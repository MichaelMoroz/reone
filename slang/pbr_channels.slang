/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// PBR as a provider, not a renderer: shades into the tracer's channel contract
// (noiseFree, demodulated diffuse/specular/direct, surface factors, viewZ) and
// the shared composite assembles it.
//
//   final = noiseFree + (diffuse + directDiffuse) * diffFactor + specular * specFactor
//
// Surface factors come from lib/surface_model.slang, so the demodulated
// channels carry no albedo, metalness or Fresnel - those live in the factors.
//
// What stays PBR's own is the provider: shadow maps, lightmap and SSAO, and the
// analytic lobes plus IBL. The tracer answers each with a ray instead.

import uniforms;
import lib.math;
import lib.gbuf;
import lib.gbuf_resolve;
import lib.envmap;
import lib.shadow;
import lib.scene_schema;
import lib.material_ops;
import lib.assemble;
import lib.surface_model;
import lib.direct_light;
import lib.skycube;
import lib.brdf;
import lib.material_bindless;

// The rest of the persistent resolve table; the G-buffer units are lib/gbuf_resolve's.
[[vk::binding(13, 1)]] Sampler2D sBRDFLUT;
[[vk::binding(16, 1)]] SamplerCubeArray sIrradianceMapArray;
[[vk::binding(17, 1)]] SamplerCubeArray sPrefilteredEnvMapArray;

[[vk::binding(1, 2)]] StructuredBuffer<uint> mergedMaterialIds;

// Set 3: the sky cube and the channel outputs, in descriptors.h's order; the
// composite's image (binding 0) belongs to a later dispatch.
[[vk::binding(1, 3)]] SamplerCube sSkyCube;
[[vk::binding(2, 3)]] RWTexture2D<float4> outNoiseFree;
[[vk::binding(3, 3)]] RWTexture2D<float4> outDiffuse;
[[vk::binding(4, 3)]] RWTexture2D<float4> outSpecular;
[[vk::binding(5, 3)]] RWTexture2D<float4> outDirectDiffuse;
[[vk::binding(6, 3)]] RWTexture2D<float4> outDiffFactor;
[[vk::binding(7, 3)]] RWTexture2D<float4> outSpecFactor;
[[vk::binding(8, 3)]] RWTexture2D<float> outViewZ;
// DLSS-RR's guide pair, the same image the tracer fills: a plain world normal
// and linear roughness serve both providers. Unwritten in Retro.
[[vk::binding(9, 3)]] RWTexture2D<float4> outNormalRoughness;

/** Mirrors ResolvePushConstants in scenepipeline.cpp; the same block the
    single-image resolve takes, unchanged. */
struct ResolvePushConstants {
    uint flags;
    float thinTransmission;
    float albedoGamma;
    float pad0;
    float lightmapIntensity;
    float emitterRadiusRatio;
    float directIntensity;
    float sunIntensity;
    float skyIntensity;
    float skyGamma;
    float pad;
};
[[vk::push_constant]] ResolvePushConstants pushConstants;

static const uint kResolveFlagSky = 1u;
static const uint kResolveFlagSSAO = 2u;
static const uint kResolveFlagParityDirect = 16u;
static const float kMaxReflectionLOD = 4.0;
static const float kSsaoMaxDistance = 500.0;

float3 reconstructViewPos(float2 uv) {
    return gbufViewPosition(uv, sGBufDepth.SampleLevel(uv, 0.0).r, globalUniforms.projectionInv);
}

/** Screen-space ambient occlusion; weights the indirect diffuse channel. */
float screenSpaceOcclusion(float2 uv, int2 pixel, float3 pos, float3 normal) {
    uint h = uint(pixel.x) * 73856093u ^ uint(pixel.y) * 19349663u;
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    float angle = float(h & 0xffffu) * (kPi * 2.0 / 65536.0);
    float3 random = float3(cos(angle), sin(angle), 0.0);
    float3 tangent = normalize(random - normal * dot(random, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 tbn = float3x3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < kNumSSAOSamples; ++i) {
        float3 samplePos = pos + screenEffectUniforms.ssaoSampleRadius *
                                     mul(screenEffectUniforms.ssaoSamples[i].xyz, tbn);
        float4 sampleClip = mul(globalUniforms.projection, float4(samplePos, 1.0));
        float2 sampleUV = sampleClip.xy / sampleClip.w;
        sampleUV = float2(sampleUV.x * 0.5 + 0.5, 0.5 - sampleUV.y * 0.5);
        float sceneZ = reconstructViewPos(sampleUV).z;
        float rangeCheck = smoothstep(
            0.0, 1.0, screenEffectUniforms.ssaoSampleRadius / abs(pos.z - sceneZ));
        occlusion += (sceneZ >= samplePos.z + screenEffectUniforms.ssaoBias ? 1.0 : 0.0) *
                     rangeCheck;
    }
    return 1.0 - (occlusion / float(kNumSSAOSamples)) *
                     (1.0 - smoothstep(0.0, kSsaoMaxDistance, abs(pos.z)));
}

/** Everything downstream needs from one PBR direct-light evaluation. Private
    to this provider now: the debug view reads the channel this pass writes
    instead of re-deriving the light, so nothing else evaluates it. */
struct PbrDirectLight {
    bool direct;
    float3 direction;
    /** Linear source radiance after intensity and this mode's visibility. */
    float3 radiance;
    /** Demodulated Lambertian diffuse, including 1/pi and thin transmission. */
    float3 diffuse;
    /** The legacy ambient-only fill, before material ambient is applied. */
    float3 ambientRadiance;
};

PbrDirectLight pbrDirectLight(GlobalUniformsLight light,
                              float3 viewPos, float3 worldPos,
                              float3 normal, bool thinSurface,
                              bool shadowsEnabled, float thinTransmission,
                              float emitterRadiusRatio,
                              float directIntensity, float sunIntensity) {
    PbrDirectLight result = { false, float3(0.0, 0.0, 1.0), float3(0.0), float3(0.0), float3(0.0) };

    // The shared module owns what the light is; only the visibility is PBR's.
    DirectLightSample sample = sampleDirectLight(light, worldPos, emitterRadiusRatio,
                                                 directIntensity, sunIntensity);
    result.direction = sample.direction;
    // Local lights only: the tracer promotes a directional ambient-only record
    // to a sampled sun, and both renderers must agree on what it is.
    if (!sample.directional && light.ambientOnly != 0) {
        result.ambientRadiance = directLightColor(light) *
                                 sphereLightAttenuation(light.radius, sample.distance,
                                                        emitterRadiusRatio);
        return result;
    }
    // Past the caster budget a directional is applied unshadowed, not deleted:
    // the two renderers may differ in how occlusion is answered, not in what
    // light exists.
    result.direct = true;

    float visibility = 1.0;
    if (shadowsEnabled && light.shadowSlot >= 0) {
        // A linear occlusion fraction, like the tracer's transmittance; not a colour.
        visibility = getShadowFrom(light.shadowSlot, viewPos, worldPos,
                                   normal, thinSurface);
    }
    result.radiance = sample.radiance * visibility;
    result.diffuse = directLightDiffuse(result.radiance, normal, result.direction,
                                        thinSurface, thinTransmission);
    return result;
}

/** One surface's contribution to the shared channels. */
struct Channels {
    float3 noiseFree;
    float3 diffuse;       // demodulated indirect
    float3 directDiffuse; // demodulated primary direct
    float3 specular;      // demodulated, divided by specFactor
    float3 diffFactor;
    float3 specFactor;
    float viewZ;
    /** World normal and linear roughness, the guide pair DLSS-RR reads. */
    float3 normal;
    float roughness;
};

/** A terminator - sky miss, sky shell, backdrop, parity black - carries all its
    radiance in noiseFree and nothing on the other channels. */
Channels terminator(float3 noiseFree) {
    // No surface: the normal/roughness guide matches the tracer's miss exactly.
    Channels c = { noiseFree, float3(0.0), float3(0.0), float3(0.0), float3(1.0), float3(1.0),
                   kAssembleInfDistance, float3(0.0), 1.0 };
    return c;
}

Channels shadePixel(float2 uv, int2 pixel) {
    uint triangleId = gbufTriangleId(pixel);

    // Nothing drawn here: the sky's own branch, matching the tracer's miss. The
    // sky dial is shared, so this radiance equals the tracer's on the same
    // pixel. Parity blacks the sky on both sides.
    if (triangleId == kNoTriangle) {
        if ((pushConstants.flags & kResolveFlagSky) == 0 ||
            (pushConstants.flags & kResolveFlagParityDirect) != 0) {
            return terminator(float3(0.0));
        }
        float3 direction = skyDirection(uv, globalUniforms.projectionInv,
                                        globalUniforms.viewInv,
                                        globalUniforms.cameraPosition.xyz);
        return terminator(skyRadiance(sSkyCube, direction, pushConstants.skyIntensity, pushConstants.skyGamma));
    }
    InstanceMaterial material = instanceMaterials[mergedMaterialIds[triangleId]];
    GBufferSurface g = readGBufferSurface(uv);
    float4 mainTexSample = g.diffuse;
    float4 lightmapSample = g.lightmap;
    float4 selfIllumSample = g.selfIllum;
    float3 eyePos = g.eyePosition;
    float3 eyeNormal = g.eyeNormal;
    float3 worldPos = g.worldPosition;
    float3 normal = g.normal;
    float3 V = normalize(globalUniforms.cameraPosition.xyz - worldPos);

    // The shared material chain: the same decode, curation and category
    // override the tracer runs, so albedo/roughness/metallic match its primary.
    MaterialSurface surface = resolveMaterial(material, mainTexSample.rgb, mainTexSample.a, 1.0,
                                              pushConstants.albedoGamma);
    float3 albedo = surface.albedo;
    float metallic = surface.metallic;
    float roughness = surface.roughness;
    float3 materialAmbient = gammaToLinear(material.ambientColor.rgb);

    bool envmapped = g.envmapped, hasLightmap = g.hasLightmap, staticObject = g.staticObject,
         shadowsEnabled = g.shadowsEnabled, thinSurface = g.thin;

    // Parity: the bare shared unoccluded sum, into noiseFree, and the sky class
    // blacked, exactly as the tracer's parity branch. Hash-equal by construction.
    if ((pushConstants.flags & kResolveFlagParityDirect) != 0) {
        if (material.surfaceType == 2) {
            return terminator(float3(0.0));
        }
        return terminator(parityDirectColor(albedo, worldPos, normal, thinSurface,
                                            pushConstants.thinTransmission,
                                            pushConstants.emitterRadiusRatio,
                                            pushConstants.directIntensity,
                                            pushConstants.sunIntensity));
    }

    // The unlit-emissive class: sky shell and painted backdrops are radiance,
    // terminating, on their own dials - assembleSkyClass, the same call the
    // tracer makes, the same shared dials.
    if ((material.featureMask & kMaterialSky) != 0 || material.surfaceType == 2) {
        return terminator(assembleSkyClass(pow(max(mainTexSample.rgb, 0.0), float3(material.emission.y)),
                                           material.emission.x));
    }

    // The shared surface model, folded into the demodulation factors so the
    // composite reconstructs the same surface.
    float specular = surface.specular;
    float f0 = surface.f0;
    float3 diffFactor, specFactor;
    materialFactors(normal, V, albedo, metallic, specular, f0, roughness, diffFactor, specFactor);

    float ao = (pushConstants.flags & kResolveFlagSSAO) != 0
                   ? screenSpaceOcclusion(uv, pixel, eyePos, eyeNormal)
                   : 1.0;

    // Indirect diffuse: the bake is the indirect light here, on its own dial;
    // demodulated, the composite puts albedo back through diffFactor.
    float3 eyeR = reflect(normalize(eyePos), eyeNormal);
    int envLayer = int(round(selfIllumSample.a * 255.0));
    float3 environment = float3(0.0);
    float3 irradiance = hasLightmap
                            ? gammaToLinear(lightmapSample.rgb) * pushConstants.lightmapIntensity
                        : staticObject
                            ? float3(pushConstants.lightmapIntensity)
                            : materialAmbient *
                                  gammaToLinear(globalUniforms.worldAmbientColor.rgb);
    if (envmapped) {
        irradiance += gammaToLinear(
            sIrradianceMapArray.SampleLevel(float4(eyeR, float(envLayer)), 0.0).rgb);
        environment = gammaToLinear(
            sPrefilteredEnvMapArray.SampleLevel(float4(eyeR, float(envLayer)),
                                                roughness * kMaxReflectionLOD).rgb);
    }

    float NdotV = max(0.0, dot(normal, V));
    SpecularLayer layer = materialSpecular(albedo, metallic, specular, f0);
    SpecularEnergy energy = specularEnergy(layer, NdotV, roughness);

    // Direct light, each caster occluded by its own map: demodulated Lambert
    // plus the analytic GGX lobe, demodulated by specFactor at the end.
    float3 directDiffuse = float3(0.0);
    float3 directS = float3(0.0);
    for (int i = 0; i < globalUniforms.numLights; ++i) {
        GlobalUniformsLight light = globalUniforms.lights[i];
        PbrDirectLight direct = pbrDirectLight(
            light, eyePos, worldPos, normal, thinSurface, shadowsEnabled,
            pushConstants.thinTransmission, pushConstants.emitterRadiusRatio,
            pushConstants.directIntensity, pushConstants.sunIntensity);
        if (!direct.direct) {
            irradiance += materialAmbient * direct.ambientRadiance;
            continue;
        }
        directDiffuse += direct.diffuse;
        directS += specularGGX(normal, V, direct.direction, roughness, layer) *
                   energy.compensation * direct.radiance;
    }

    // Ambient specular (split-sum prefiltered environment) on the alpha share,
    // and the authored sharp mirror on the (1 - alpha) share.
    float3 ambientS = float3(0.0);
    float3 mirror = float3(0.0);
    if (envmapped) {
        float3 Fr = fresnelSchlickRoughness(NdotV, layer.f0, roughness);
        float2 brdf = sBRDFLUT.SampleLevel(float2(NdotV, roughness), 0.0).xy;
        ambientS = mainTexSample.a * environment * layer.weight * (Fr * brdf.x + brdf.y) *
                   energy.compensation;
        mirror = gammaToLinear(sampleAuthoredMirrorLevel0(
                     material,
                     authoredMirrorReflection(worldPos, globalUniforms.cameraPosition.xyz,
                                              normal))) *
                 (1.0 - mainTexSample.a);
    }

    Channels c;
    // Emission is unmultiplied radiance into noiseFree, on the tracer's rule.
    float3 radianceTexel = pow(max(mainTexSample.rgb, 0.0), float3(material.emission.y));
    c.noiseFree = assembleEmission(gammaToLinear(selfIllumSample.rgb), radianceTexel,
                                   material.curatedEmission, material.emission.x);
    c.diffuse = ao * irradiance;
    c.directDiffuse = max(directDiffuse, float3(0.0));
    c.specular = (max(directS, float3(0.0)) + ao * ambientS + mirror) /
                 max(specFactor, float3(1e-4));
    c.diffFactor = diffFactor;
    c.specFactor = specFactor;
    c.viewZ = eyePos.z;
    c.normal = normal;
    c.roughness = roughness;
    return c;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void channelsMain(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outNoiseFree.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) {
        return;
    }
    int2 pixel = int2(dispatchId.xy);
    // The half-texel offset lands the sample on the pixel centre the tracer's
    // primary and the single-image resolve both use.
    float2 uv = (float2(pixel) + 0.5) / float2(width, height);

    Channels c = shadePixel(uv, pixel);

    // Every pixel writes every channel: a storage image keeps stale data
    // otherwise, and the composite reads all of them.
    outNoiseFree[pixel] = float4(c.noiseFree, 1.0);
    outDiffuse[pixel] = float4(c.diffuse, 0.0);
    outSpecular[pixel] = float4(c.specular, 0.0);
    outDirectDiffuse[pixel] = float4(c.directDiffuse, 0.0);
    outDiffFactor[pixel] = float4(c.diffFactor, 1.0);
    outSpecFactor[pixel] = float4(c.specFactor, 1.0);
    outViewZ[pixel] = c.viewZ;
    outNormalRoughness[pixel] = float4(c.normal, c.roughness);
}
