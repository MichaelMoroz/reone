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
import lib.envmap;
import lib.shadow;
import lib.scene_schema;
import lib.material_ops;
import lib.assemble;
import lib.surface_model;
import lib.direct_light;
import lib.skycube;
// Colour and BRDF maths only; no tracing entry point is referenced.
import tracing.brdf;

// Set 1 is the persistent resolve table shared with retro_resolve; only the
// units this pass reads are declared.
[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(13, 1)]] Sampler2D sBRDFLUT;
[[vk::binding(16, 1)]] SamplerCubeArray sIrradianceMapArray;
[[vk::binding(17, 1)]] SamplerCubeArray sPrefilteredEnvMapArray;
[[vk::binding(21, 1), vk::image_format("r32ui")]] Sampler2D<uint> sGBufTriangleId;

[[vk::binding(1, 2)]] StructuredBuffer<uint> mergedMaterialIds;
[[vk::binding(2, 2)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 2)]] Sampler2D bindlessTextures[];
[[vk::binding(5, 2)]] SamplerCube bindlessTextureCubes[];

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
    float roughnessFloor;
    float lightmapIntensity;
    float emitterRadiusRatio;
    float directIntensity;
    float sunIntensity;
    float skyIntensity;
    float backdropIntensity;
    float emissiveIntensity;
};
[[vk::push_constant]] ResolvePushConstants pushConstants;

static const uint kResolveFlagSky = 1u;
static const uint kResolveFlagSSAO = 2u;
static const uint kResolveFlagParityDirect = 16u;
static const uint kResolveMaskSky = 1u << 24;
static const uint kNoTriangle = 0xffffffffu;
static const uint kNoTexture = 0xffffffffu;
static const float kMaxReflectionLOD = 4.0;
static const float kSsaoMaxDistance = 500.0;
static const float kGamma = 2.2;

/** Authored, display-referred colour to linear. The one decode this pass runs
    on material colours and the fog swatch; the encode is the post pass's. */
float3 gammaToLinear(float3 rgb) {
    return pow(max(rgb, 0.0), float3(kGamma));
}

/** View-space position from the depth the geometry pass left; see the note in
    the SSR module, which reconstructs the same way. */
float3 reconstructViewPos(float2 uv) {
    float depth = sGBufDepth.SampleLevel(uv, 0.0).r;
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth);
    float4 viewPos = mul(globalUniforms.projectionInv, float4(ndc, 1.0));
    return viewPos.xyz / viewPos.w;
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

float3 sampleAuthoredMirror(InstanceMaterial material, float3 worldReflection) {
    if (material.envMapCube != kNoTexture) {
        return bindlessTextureCubes[NonUniformResourceIndex(material.envMapCube)]
            .SampleLevel(worldReflection, 0.0).rgb;
    }
    float2 uv = authoredMirrorUV(worldReflection);
    return bindlessTextures[NonUniformResourceIndex(material.envMap)]
        .SampleLevel(uv, 0.0).rgb;
}

/** The one term with no tracer counterpart: the ambient lobe's Fresnel takes
    a roughness ceiling, which the punctual ptFresnelSchlick does not. */
float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    float3 ceiling = max(F0, float3(1.0 - roughness));
    return F0 + (ceiling - F0) * pow(saturate(1.0 - cosTheta), 5.0);
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
    /** Cosine against the unflipped normal, for the specular lobe. */
    float specularNdotL;
    /** The legacy ambient-only fill, before material ambient is applied. */
    float3 ambientRadiance;
};

PbrDirectLight pbrDirectLight(GlobalUniformsLight light,
                              float3 viewPos, float3 worldPos,
                              float3 normal, bool thinSurface,
                              bool shadowsEnabled, float thinTransmission,
                              float emitterRadiusRatio,
                              float directIntensity, float sunIntensity) {
    PbrDirectLight result = { false, float3(0.0, 0.0, 1.0), float3(0.0), float3(0.0), 0.0, float3(0.0) };

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
    result.specularNdotL = max(0.0, dot(normal, result.direction));
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
    float4 mainTexSample = sGBufDiffuse.SampleLevel(uv, 0.0);
    float4 lightmapSample = sGBufLightmap.SampleLevel(uv, 0.0);
    float4 selfIllumSample = sGBufSelfIllum.SampleLevel(uv, 0.0);
    uint triangleId = sGBufTriangleId.Load(int3(pixel, 0)).r;

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
        return terminator(skyRadiance(sSkyCube, direction, pushConstants.skyIntensity));
    }
    InstanceMaterial material = instanceMaterials[mergedMaterialIds[triangleId]];

    float3 eyePos = reconstructViewPos(uv);
    float3 eyeNormal = normalize(2.0 * sGBufEyeNormal.SampleLevel(uv, 0.0).xyz - 1.0);
    float3 worldPos = mul(globalUniforms.viewInv, float4(eyePos, 1.0)).xyz;
    float3 normal = normalize(mul(globalUniforms.viewInv, float4(eyeNormal, 0.0)).xyz);
    float3 V = normalize(globalUniforms.cameraPosition.xyz - worldPos);

    // The shared material chain: the same decode, curation and category
    // override the tracer runs, so albedo/roughness/metallic match its primary.
    MaterialSurface surface = resolveMaterial(material, mainTexSample.rgb, mainTexSample.a,
                                              pushConstants.albedoGamma,
                                              pushConstants.roughnessFloor);
    float3 albedo = surface.albedo;
    float metallic = surface.metallic;
    float roughness = surface.roughness;
    float3 materialAmbient = gammaToLinear(material.ambientColor.rgb);

    bool envmapped, hasLightmap, staticObject, shadowsEnabled, fogEnabled, thinSurface;
    unpackGeometryFeatures(lightmapSample.a, envmapped, hasLightmap, staticObject,
                           shadowsEnabled, fogEnabled, thinSurface);

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
    bool skyShell = (material.featureMask & kResolveMaskSky) != 0;
    if (skyShell || material.surfaceType == 2) {
        return terminator(assembleSkyClass(skyShell, albedo, pushConstants.skyIntensity,
                                           pushConstants.backdropIntensity));
    }

    // The shared surface model. envStrength is the tracer's: the authored mirror
    // fraction (1 - alpha) scaled by the category override, folded into the
    // demodulation factors so the composite reconstructs the same surface.
    float envStrength = (envmapped ? (1.0 - mainTexSample.a) : 0.0) *
                        material.overrideParams.z;
    float3 diffFactor, specFactor;
    materialFactors(normal, V, albedo, metallic, envStrength, roughness,
                    diffFactor, specFactor);

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
    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float a = roughness * roughness;
    float a2 = a * a;
    float k = (roughness + 1.0) * (roughness + 1.0) * 0.125;

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
        float3 L = direct.direction;
        float3 H = normalize(V + L);
        float NdotH = max(0.0, dot(normal, H));
        float VdotH = max(0.0, dot(V, H));
        float D = ptDistributionGGX(NdotH * NdotH, a2);
        float G = ptGeometrySmith(direct.specularNdotL, NdotV, k);
        float3 Fs = ptFresnelSchlick(VdotH, F0);
        float3 spec = (D * G * Fs) / max(1e-4, 4.0 * direct.specularNdotL * NdotV);
        directDiffuse += direct.diffuse;
        directS += spec * direct.radiance * direct.specularNdotL;
    }

    // Ambient specular (split-sum prefiltered environment) on the alpha share,
    // and the authored sharp mirror on the (1 - alpha) share.
    float3 ambientS = float3(0.0);
    float3 mirror = float3(0.0);
    if (envmapped) {
        float3 Fr = fresnelSchlickRoughness(NdotV, F0, roughness);
        float2 brdf = sBRDFLUT.SampleLevel(float2(NdotV, roughness), 0.0).xy;
        ambientS = mainTexSample.a * environment * (Fr * brdf.x + brdf.y);
        mirror = gammaToLinear(sampleAuthoredMirror(
                     material,
                     authoredMirrorReflection(worldPos, globalUniforms.cameraPosition.xyz,
                                              normal))) *
                 (1.0 - mainTexSample.a);
    }

    Channels c;
    // Emission is unmultiplied radiance into noiseFree, on the tracer's rule.
    float3 radianceTexel = gammaToLinear(mainTexSample.rgb);
    c.noiseFree = assembleEmission(gammaToLinear(selfIllumSample.rgb), radianceTexel,
                                   material.curatedEmission, pushConstants.emissiveIntensity);
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
