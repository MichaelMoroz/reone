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
// The deferred resolve: read the G-buffer, light once per pixel, write the
// scene output. The Vulkan counterpart of glsl/f_pbr_combine.glsl.
//
// Ported faithfully for everything the Vulkan pipeline can currently supply:
// the scene light list, the GGX/Smith/Fresnel BRDF, world ambient, gamma
// handling, the geometry feature bits packed into the lightmap alpha, and fog.
//
// The important part is the shape rather than the lighting. SurfaceParams is
// filled here from a G-buffer, and a path tracer will fill the same struct from
// a hit record; everything downstream of it is shared. Keep that boundary.
//
// A compute dispatch rather than a full-screen triangle. Nothing about the
// lighting wanted a rasterizer: every pixel is independent, the output is
// written once, and a dispatch can therefore both fold in screen-space
// occlusion - which needs a second read of depth and normals, not a second
// pass - and skip the attachment machinery. The retro resolve deliberately
// stays a fragment pass: it is the original's model, it has no occlusion term
// to fold in, and converting it would buy nothing while putting the one thing
// this project treats as a fixed reference through a rewrite.
//
// The one consequence to know: a compute shader has no implicit derivatives, so
// every texture read here takes an explicit level. Every target it samples is
// mip-free, where level zero is what an implicit lookup resolved to anyway; the
// exception is the authored 2D mirror in sampleAuthoredMirror, which now takes
// its base level exactly as the cube branch beside it always did.

import uniforms;
import lib.math;
import lib.gbuf;
import lib.shadow;
import lib.scene_schema;
import lib.material_ops;
import lib.skycube;

/**
 * The resolved scene colour, read by the screen-space reflection pass alone.
 *
 * That pass runs as a second dispatch over the image this one produces, which
 * is the whole reason it is a second dispatch: reflections march the screen and
 * need the lit colour of the pixel they land on, and a kernel cannot read the
 * image it is still writing.
 */
[[vk::binding(0, 1)]] Sampler2D sSceneColor;
[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(13, 1)]] Sampler2D sBRDFLUT;
[[vk::binding(16, 1)]] SamplerCubeArray sIrradianceMapArray;
[[vk::binding(17, 1)]] SamplerCubeArray sPrefilteredEnvMapArray;
[[vk::binding(21, 1), vk::image_format("r32ui")]] Sampler2D<uint> sGBufTriangleId;
// The merged triangle-to-material table, the same array scene_draw.slang read
// to decide what to shade. This pass now takes the material through it rather
// than from a G-buffer target of its own: the triangle id is stored because the
// tracer needs the geometry, and once it is there a material target is a second
// copy of something already on the pixel.
[[vk::binding(1, 2)]] StructuredBuffer<uint> mergedMaterialIds;
[[vk::binding(2, 2)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 2)]] Sampler2D bindlessTextures[];
[[vk::binding(5, 2)]] SamplerCube bindlessTextureCubes[];
// Set 3 is the resolve set: the image this dispatch writes, and the baked sky
// cube both resolves read. The output is a storage image because a dispatch has
// no attachments; it is acquired per frame because the scene output and the
// tail target exchange identities every time a tail pass runs.
[[vk::binding(0, 3)]] RWTexture2D<float4> outImage;
[[vk::binding(1, 3)]] SamplerCube sSkyCube;

/** Mirrors ResolvePushConstants in scenepipeline.cpp. */
struct ResolvePushConstants {
    /** Bit 0: sky. Bit 1: SSAO. Bit 2: uncovered pixels keep alpha zero. */
    uint flags;
    /** Fraction of light a thin surface passes to its far side. */
    float thinTransmission;
    /** Exponent authored surface colour is decoded with; shared with the tracer. */
    float albedoGamma;
    /** Lowest roughness any surface may take; shared with the tracer. */
    float roughnessFloor;
    /**
     * Strength of the baked irradiance. Not shared with the tracer's dial of
     * the same name, and the only material-facing number that is not: see the
     * irradiance term below.
     */
    float lightmapIntensity;
};
[[vk::push_constant]] ResolvePushConstants pushConstants;

static const uint kResolveFlagSky = 1u;
static const uint kResolveFlagSSAO = 2u;
static const uint kResolveFlagTransparentOutput = 4u;

static const uint kNoTriangle = 0xffffffffu;
static const uint kNoTexture = 0xffffffffu;

/** Roughness spans this many prefiltered mips; see VulkanPBRTextures. */
static const float kMaxReflectionLOD = 4.0;

/** View-space distance past which the occlusion kernel measures only noise. */
static const float kSsaoMaxDistance = 500.0;

static const float kGamma = 2.2;

/**
 * A surface's shading parameters at a point, independent of how the point was
 * reached. The raster resolve reaches it through a G-buffer, a path tracer will
 * reach it through a hit record; neither the producer nor the BSDF needs to know
 * which.
 *
 * Odyssey assets do not carry these directly - they carry diffuse, lightmap,
 * envmap and bump slots plus ambient/diffuse/self-illum colours - so something
 * has to map between the two. That mapping wants to move into the material
 * buffer of section 5.6, computed once per material rather than per fragment.
 * Keeping it behind this struct means that move changes where SurfaceParams
 * comes from and nothing that consumes it.
 */
struct SurfaceParams {
    float3 albedo;
    float alpha;
    float3 normal;    // world space
    float3 emissive;
    float roughness;
    float metallic;
    /** Baked irradiance, or 1 where a surface carries no lightmap. */
    float3 lightmap;
    bool hasLightmap;
};

SurfaceParams defaultSurface() {
    SurfaceParams s;
    s.albedo = float3(1.0);
    s.alpha = 1.0;
    s.normal = float3(0.0, 0.0, 1.0);
    s.emissive = float3(0.0);
    // Odyssey materials carry no roughness or metallic channel. These are the
    // stand-ins the PBR pipeline already assumes; deriving something better from
    // the asset is a separate question - see section 5.4.
    s.roughness = 1.0;
    s.metallic = 0.0;
    s.lightmap = float3(1.0);
    s.hasLightmap = false;
    return s;
}



// Only one direction is needed here now. Odyssey's authored colours - texels,
// material colours, light colours, the fog colour - are display-referred and
// come in through this; the resolve then stays linear all the way out, and the
// encode belongs to the shared post-process pass alone.
float3 gammaToLinear(float3 rgb) {
    return pow(max(rgb, 0.0), float3(kGamma));
}

/**
 * View-space position of the pixel, from the depth the geometry pass left.
 *
 * Two Vulkan-specific corrections, both silent if wrong - the picture stays
 * plausible and only the lighting is misplaced:
 *
 * Depth is already the 0..1 that clip space uses here, so it goes into NDC
 * unchanged. OpenGL's -1..1 conversion would halve the reconstructed range.
 *
 * NDC y is negated. The geometry pass rasterises through a negative-height
 * viewport, which is what lets an OpenGL projection produce an upright image;
 * that flip has to be undone to recover the clip-space y a fragment really had.
 */
float3 reconstructViewPos(float2 uv) {
    float depth = sGBufDepth.SampleLevel(uv, 0.0).r;
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth);
    float4 viewPos = mul(globalUniforms.projectionInv, float4(ndc, 1.0));
    return viewPos.xyz / viewPos.w;
}

/**
 * Screen-space ambient occlusion, folded into the resolve rather than run as a
 * pass of its own.
 *
 * It needs depth and normals and nothing else, both of which this dispatch has
 * already read, and it produces one scalar that is consumed a few lines later.
 * As a separate full-screen pass it cost a target, a blur to hide its noise and
 * a third read of the same G-buffer; here it costs a loop.
 *
 * The rotation comes from a hash of the pixel rather than from the 4x4 noise
 * texture the old fragment pass sampled: that texture is not bound in either
 * resolve's descriptor set, and a hash is one less resource to keep in step for
 * the same purpose - decorrelating neighbouring kernels so the occlusion reads
 * as noise rather than as banding.
 */
float screenSpaceOcclusion(float2 uv, int2 pixel, float3 pos, float3 normal) {
    // Wang-style integer hash: cheap, and its low bits vary between adjacent
    // pixels, which is the whole requirement.
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
        // GLSL mat3 takes these basis vectors as columns. Slang lowers this
        // constructor as the same SPIR-V columns, so multiply the sample from
        // the left to obtain tangent*x + bitangent*y + normal*z.
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
    // Faded out with distance exactly as the fragment pass faded it: a kernel
    // whose world radius is a pixel wide measures nothing but depth precision.
    return 1.0 - (occlusion / float(kNumSSAOSamples)) *
                     (1.0 - smoothstep(0.0, kSsaoMaxDistance, abs(pos.z)));
}

float lightAttenuationQuadratic(GlobalUniformsLight light, float distance) {
    float d = light.radius + distance;
    return (light.radius * light.radius) / (d * d);
}

float3 sampleAuthoredMirror(InstanceMaterial material, float3 eyeReflection) {
    if (material.envMapCube != kNoTexture) {
        // This fullscreen pass cannot derive a stable cube LOD from its
        // quantized normal. Minified-surface sharpness is a separate policy;
        // the authored mirror deliberately uses its base level here.
        return bindlessTextureCubes[NonUniformResourceIndex(material.envMapCube)]
            .SampleLevel(eyeReflection, 0.0).rgb;
    }
    float3 r = eyeReflection;
    float m = 2.0 * sqrt(r.x * r.x + r.y * r.y +
                         (r.z + 1.0) * (r.z + 1.0));
    float2 uv = float2(r.x / m + 0.5, r.y / m + 0.5);
    // Base level, like the cube branch above. A dispatch has no implicit
    // derivatives at all, so there is no mip selection to be had here; this is
    // the one read in the resolve whose value the move to compute changes, and
    // it changes it in the direction the cube branch was already committed to.
    return bindlessTextures[NonUniformResourceIndex(material.envMap)]
        .SampleLevel(uv, 0.0).rgb;
}

float distributionGGX(float NdotH2, float a2) {
    float d = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float geometrySchlick(float NdotV, float k) {
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(float NdotL, float NdotV, float k) {
    return geometrySchlick(NdotL, k) * geometrySchlick(NdotV, k);
}

float3 fresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    float3 ceiling = max(F0, float3(1.0 - roughness));
    return F0 + (ceiling - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float getFog(float3 worldPos) {
    float distance = length(worldPos - globalUniforms.cameraPosition.xyz);
    float f = (globalUniforms.fogFar - distance) /
              (globalUniforms.fogFar - globalUniforms.fogNear);
    return 1.0 - saturate(f);
}

/** One pixel of the resolved image, as linear scene-referred radiance. */
float3 resolveSurface(float2 uv, int2 pixel) {
    float4 mainTexSample = sGBufDiffuse.SampleLevel(uv, 0.0);
    float4 lightmapSample = sGBufLightmap.SampleLevel(uv, 0.0);
    float4 selfIllumSample = sGBufSelfIllum.SampleLevel(uv, 0.0);
    uint triangleId = sGBufTriangleId.Load(int3(pixel, 0)).r;

    // Nothing was drawn here, and nothing wrote depth either - the triangle id
    // and the depth come from one fragment - so this is exactly the set the sky
    // composite pass used to paint over, and it is now the sky's own branch.
    // Lighting it would tint the whole background; without a bake the cube is
    // black by construction, so skipping it writes the same black as before.
    if (triangleId == kNoTriangle) {
        if ((pushConstants.flags & kResolveFlagSky) == 0) {
            return float3(0.0);
        }
        float3 direction = skyDirection(uv, globalUniforms.projectionInv,
                                        globalUniforms.viewInv,
                                        globalUniforms.cameraPosition.xyz);
        // The bake already stores linear radiance and this pass writes linear,
        // so the cube is composited at identity; the encode is the post pass's.
        return skyRadiance(sSkyCube, direction, 1.0);
    }
    InstanceMaterial material = instanceMaterials[mergedMaterialIds[triangleId]];

    float3 eyePos = reconstructViewPos(uv);
    float3 eyeNormal = normalize(2.0 * sGBufEyeNormal.SampleLevel(uv, 0.0).xyz - 1.0);
    float3 worldPos = mul(globalUniforms.viewInv, float4(eyePos, 1.0)).xyz;
    float3 normal = normalize(mul(globalUniforms.viewInv, float4(eyeNormal, 0.0)).xyz);

    float3 V = normalize(globalUniforms.cameraPosition.xyz - worldPos);

    // The same chain the tracer runs, from lib/material_ops.slang: the decode,
    // the curated per-object operations and the per-category calibration
    // override. This resolve used to run none of it - albedo was the raw texel
    // and roughness a hardcoded floor over the alpha - so every object a
    // curation or category dial touched was a different brightness here than
    // in path tracing, and the dials themselves did nothing in this mode.
    MaterialSurface surface = resolveMaterial(material, mainTexSample.rgb, mainTexSample.a,
                                              pushConstants.albedoGamma,
                                              pushConstants.roughnessFloor);
    float3 albedo = surface.albedo;
    // Odyssey assets carry neither metalness nor roughness. Diffuse alpha is
    // the authored mirror strength, handled separately below exactly as the
    // original renderer did; it is not a second F0 for the IBL lobe.
    float metallic = surface.metallic;
    float roughness = surface.roughness;
    float3 emission = gammaToLinear(selfIllumSample.rgb);
    float3 materialAmbient = gammaToLinear(material.ambientColor.rgb);
    float3 materialDiffuse = gammaToLinear(material.diffuseColor.rgb);
    // Screen-space occlusion, when it is switched on. With it off this is the
    // neutral one the resolve has always used and the branch is not taken, so
    // the frame is the frame that was rendered before it existed.
    float ao = (pushConstants.flags & kResolveFlagSSAO) != 0
                   ? screenSpaceOcclusion(uv, pixel, eyePos, eyeNormal)
                   : 1.0;

    bool envmapped;
    bool hasLightmap;
    bool staticObject;
    bool shadowsEnabled;
    bool fogEnabled, thinSurface;
    unpackGeometryFeatures(lightmapSample.a, envmapped, hasLightmap, staticObject,
                           shadowsEnabled, fogEnabled, thinSurface);

    float shadowFactor = 1.0;
    if (shadowsEnabled) {
        shadowFactor = getShadow(eyePos, worldPos, normal, thinSurface);
    }
    // ShadowOpacity was authored for the original gamma-space renderer. PBR
    // applies lighting in linear space, so convert the display visibility to
    // an energy multiplier before using it on the direct BRDF terms.
    //
    // Direct terms only. It used to scale the ambient/IBL lobes as well, and
    // since irradiance is the LIGHTMAP on lightmapped geometry and the flat
    // materialAmbient * worldAmbient product everywhere else, that made a
    // depth test against one light attenuate baked and ambient light. That is
    // what darkened lightmapped rooms in this mode while retro - whose
    // lightmapped branch never took the shadow term - kept the authored tone.
    float directVisibility = pow(shadowFactor, kGamma);
    float fog = fogEnabled ? getFog(worldPos) : 0.0;

    float NdotV = max(0.0, dot(normal, V));
    float3 F0 = lerp(float3(0.04), albedo, metallic);

    // Odyssey generated eye-space reflection coordinates per vertex from the
    // geometric normal. A deferred resolve necessarily does this per pixel
    // from the normal-mapped, quantized G-buffer normal instead.
    float3 R = reflect(-V, normal);
    float3 eyeR = reflect(normalize(eyePos), eyeNormal);
    int envLayer = int(round(selfIllumSample.a * 255.0));

    float3 environment = float3(0.0);
    // The path tracer deliberately does not add this flat term: outdoors its
    // sky hemisphere integral already is ambient, so adding both doubles it.
    // Interiors consequently lose this small raster-only baseline; direct
    // scene lights are the intentionally shared illumination there.
    // Static geometry with no lightmap authored reads as a white bake, not
    // as an absence of light: whole exteriors ship without lightmaps (the
    // Tatooine dune sea has none at all), and calling that unlit either
    // blackens them or exempts them from shadows. Dynamic objects keep flat
    // world ambient - they were never lightmapped and the light loop shades
    // them. Mirrors the same choice in retro_resolve.slang.
    //
    // The lightmap takes its own intensity here, and it is deliberately not the
    // tracer's. This is the one dial that must differ between the two modes:
    // the bake IS the indirect light in this mode, where the tracer computes
    // that transport outright and scales the bake toward zero to avoid counting
    // it twice. Everything else about the surface is shared.
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
    float3 directD = float3(0.0);
    float3 directS = float3(0.0);

    float a = roughness * roughness;
    float a2 = a * a;
    float k = roughness + 1.0;
    k = k * k * (1.0 / 8.0);

    for (int i = 0; i < globalUniforms.numLights; ++i) {
        GlobalUniformsLight light = globalUniforms.lights[i];
        // The original excluded all but dynamicType 1 from static geometry,
        // because a lightmap already held that light and the hardware could
        // not have afforded it twice. Retro keeps that rule; the corrected
        // model keeps it only for lights that cast nothing, for one concrete
        // reason: a shadow is visible only on the light it attenuates, and
        // the shadow-casting light is exactly what this test dropped. Static
        // geometry therefore took no direct light at all, and a shadow on it
        // had nothing to darken. Letting that one light through double-counts
        // the bake's own share of it, which is bounded by the lightmap's
        // weight and is the honest price of shadows that are not painted onto
        // baked light.
        if (staticObject && light.dynamicType != 1 && light.shadowCaster == 0) {
            continue;
        }

        float3 fragToLight = light.position.xyz - worldPos;
        float lightDistance = length(fragToLight);
        // Kept exactly as the OpenGL shader has it, radius-squared cutoff and
        // all, so that any difference between the two is not this.
        if (lightDistance > light.radius * light.radius) {
            continue;
        }

        float attenuation = lightAttenuationQuadratic(light, lightDistance);
        float3 radiance = light.multiplier * attenuation * gammaToLinear(light.color.rgb);
        if (light.ambientOnly != 0) {
            irradiance += materialAmbient * radiance;
            continue;
        }

        float3 L = normalize(fragToLight);
        // Lit from whichever side the light is on. The specular lobe keeps the
        // true normal - a leaf does not transmit a highlight - so only the
        // diffuse half is two-sided, and it is dimmed by the transmission the
        // surface is assumed to have rather than being as bright as the front.
        float3 shadeNormal = thinSurface ? thinFacingNormal(normal, L) : normal;
        float transmission =
            thinSurface && dot(normal, L) < 0.0 ? pushConstants.thinTransmission : 1.0;
        float3 H = normalize(V + L);
        float NdotL = max(0.0, dot(shadeNormal, L));
        float NdotH = max(0.0, dot(normal, H));
        float VdotH = max(0.0, dot(V, H));

        float D = distributionGGX(NdotH * NdotH, a2);
        float G = geometrySmith(max(0.0, dot(normal, L)), NdotV, k);
        float3 F = fresnelSchlick(VdotH, F0);
        float3 spec = (D * G * F) / max(1e-4, 4.0 * max(0.0, dot(normal, L)) * NdotV);

        float3 kD = (1.0 - F) * (1.0 - metallic);
        directD += materialDiffuse * kD / kPi * radiance * NdotL * transmission;
        directS += spec * radiance * max(0.0, dot(normal, L));
    }

    float3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD = (1.0 - F) * (1.0 - metallic);
    float3 ambientD = kD * irradiance;

    // Ambient specular: the environment blurred to this surface's roughness,
    // weighted by the split-sum approximation's scale and bias.
    float3 ambientS = float3(0.0);
    if (envmapped) {
        float2 brdf = sBRDFLUT.SampleLevel(float2(NdotV, roughness), 0.0).xy;
        // Diffuse alpha partitions the environment response: (1 - alpha)
        // belongs to the authored mirror below, while IBL fills the remaining
        // alpha share with roughness-varying specular. The two lobes therefore
        // complement rather than double-light one another.
        ambientS = mainTexSample.a * environment * (F * brdf.x + brdf.y);
    }

    float3 selfIllumStep = step(float3(1e-4), selfIllumSample.rgb);
    float selfIllumed = max(selfIllumStep.x, max(selfIllumStep.y, selfIllumStep.z));

    // Self-illumination is a coefficient on the light reaching the surface, not
    // a colour added on top of it. Adding it instead saturates anything fully
    // self-illuminated - a sky - to white.
    // Ambient occlusion still weights the ambient lobes; the shadow term does
    // not. AO is a property of the surface's own neighbourhood and applies to
    // light arriving from everywhere, which is exactly what these lobes are.
    // Received exactly as retro receives it: one visibility over the whole lit
    // product, not a term applied to direct light alone. On content whose
    // light budget is mostly a baked lightmap, a shadow that may only touch
    // the dynamic share is a shadow nobody can see - measured at 14/255
    // against 89 for the same scene in retro. What differs between the modes
    // is the caster set, not this expression: retro's map holds characters,
    // this one holds every opaque surface, so terrain shadowing terrain is
    // present here and is geometry, not error. Self-shadowing acne is the
    // failure to watch for, and it belongs to the depth bias and the receiver
    // normal offset in lib/shadow.slang - not to this line.
    float3 color = min(float3(1.0),
                       directVisibility * (ao * ambientD +
                           max(float3(0.0), directD)) + emission) *
                   albedo;
    color += ao * ambientS;
    // A self-illuminated surface does not take a specular highlight.
    color += (1.0 - selfIllumed) * directVisibility * max(float3(0.0), directS);

    // No encode here any more. This pass hands linear scene-referred colour to
    // the tail, exactly as the tracer does, and the shared post-process pass
    // owns the one display transform. The two terms that used to be added
    // after the encode were authored in display space and are converted on the
    // way in instead, so each is still the value the artist wrote.
    //
    // Authored mirror and roughness-varying IBL are distinct. The IBL above
    // keeps the ordinary dielectric response; this add reserves (1 - diffuse
    // alpha) for the original game's sharp mirror term, so that authored
    // strength is not counted in both lobes.
    //
    // Unshadowed, like the other environment lobe. The cube is a static
    // capture of the whole surround, not of the shadow-casting light, so a
    // depth test against that one light says nothing about how much of the
    // cube this surface can see.
    if (envmapped) {
        color += gammaToLinear(sampleAuthoredMirror(material, eyeR)) *
                 (1.0 - mainTexSample.a);
    }
    // Fog now mixes energy rather than encoded values. The authored colour is
    // a display-space swatch, so it is linearised before the mix; the mix
    // itself is a light-transport interpolation and belongs in linear space in
    // this mode. Retro keeps the original's gamma-space lerp, in its own pass.
    color = lerp(color, gammaToLinear(globalUniforms.fogColor.rgb), fog);

    return color;
}

/**
 * The resolve itself.
 *
 * The half-texel offset is not decoration: a full-screen triangle interpolated
 * uv to the pixel CENTRE, so anything else here would sample the G-buffer
 * between texels and shift the whole image.
 */
[shader("compute")]
[numthreads(8, 8, 1)]
void resolveMain(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) {
        return;
    }
    int2 pixel = int2(dispatchId.xy);
    float2 uv = (float2(pixel) + 0.5) / float2(width, height);
    uint triangleId = sGBufTriangleId.Load(int3(pixel, 0)).r;
    bool covered = triangleId != kNoTriangle || (pushConstants.flags & kResolveFlagSky) != 0;
    float alpha = (pushConstants.flags & kResolveFlagTransparentOutput) != 0 && !covered ? 0.0 : 1.0;
    outImage[pixel] = float4(resolveSurface(uv, pixel), alpha);
}

// --- Screen-space reflections ---
//
// A second dispatch, reading the image the resolve wrote and writing the tail
// target, which the pipeline then exchanges for the scene output. It cannot be
// folded into the resolve the way occlusion was: a reflection lands on some
// OTHER pixel, and a kernel that read the image it is writing would be reading
// whichever neighbouring workgroups happened to have run.
//
// It reflects the lit image rather than the G-buffer product the OpenGL pass
// reflected - a hit therefore carries the shadows, the lightmap and the sky
// that pixel actually shows, which is the entire reason to reflect the resolved
// image rather than to re-derive one.
//
// Composited as a lerp weighted by the authored mirror share, never as an add.
// The resolve has already put the authored cube into these pixels at exactly
// (1 - diffuse alpha); replacing that share where the screen holds a real hit
// is a substitution, while adding would light the surface twice.

static const float kSsrMaxDistance = 100.0;
static const float kEdgeFadeStart = 0.8;

float distanceSquared(float2 a, float2 b) { float2 d = a - b; return dot(d, d); }
void swapIfGreater(inout float a, inout float b) { if (a > b) { float t = a; a = b; b = t; } }

/**
 * The DDA march of Morgan McGuire's efficient screen-space ray trace, in view
 * space, stepping in whichever screen axis the ray covers faster.
 */
bool traceScreenSpaceRay(float3 rayOrigin, float3 rayDirection, float jitter,
                         out float2 hitUV, out float numSteps) {
    hitUV = float2(0.0);
    numSteps = 0.0;
    float rayLength =
        rayOrigin.z + rayDirection.z * kSsrMaxDistance > -screenEffectUniforms.clipNear
            ? (-screenEffectUniforms.clipNear - rayOrigin.z) / rayDirection.z
            : kSsrMaxDistance;
    float3 rayEnd = rayOrigin + rayDirection * rayLength;
    float4 h0 = mul(globalUniforms.projection, float4(rayOrigin, 1.0));
    float k0 = 1.0 / h0.w;
    float3 q0 = rayOrigin * k0;
    float2 p0 = float2(h0.x * k0 * 0.5 + 0.5, 0.5 - h0.y * k0 * 0.5) *
                screenEffectUniforms.screenResolution;
    float4 h1 = mul(globalUniforms.projection, float4(rayEnd, 1.0));
    float k1 = 1.0 / h1.w;
    float3 q1 = rayEnd * k1;
    float2 p1 = float2(h1.x * k1 * 0.5 + 0.5, 0.5 - h1.y * k1 * 0.5) *
                screenEffectUniforms.screenResolution;
    p1 += distanceSquared(p0, p1) < 0.0001 ? float2(0.01) : float2(0.0);
    float2 delta = p1 - p0;
    bool permute = abs(delta.x) < abs(delta.y);
    if (permute) { delta = delta.yx; p0 = p0.yx; p1 = p1.yx; }
    float stepDir = sign(delta.x);
    float invdx = stepDir / delta.x;
    float3 dQ = (q1 - q0) * invdx;
    float dk = (k1 - k0) * invdx;
    float2 dP = float2(stepDir, delta.y * invdx);
    float stride = screenEffectUniforms.ssrPixelStride;
    dP *= stride; dQ *= stride; dk *= stride;
    p0 += (1.0 + jitter) * dP;
    q0 += (1.0 + jitter) * dQ;
    k0 += (1.0 + jitter) * dk;
    float2 p = p0;
    float3 q = q0;
    float k = k0;
    float end = p1.x * stepDir;
    float previousZMax = rayOrigin.z;
    for (float i = 0.0; i < screenEffectUniforms.ssrMaxSteps && p.x * stepDir <= end; i += 1.0) {
        float2 sceneUV = (permute ? p.yx : p) * screenEffectUniforms.screenResolutionRcp;
        float rayZMin = previousZMax;
        float rayZMax = (dQ.z * 0.5 + q.z) / (dk * 0.5 + k);
        previousZMax = rayZMax;
        swapIfGreater(rayZMin, rayZMax);
        float sceneZ = reconstructViewPos(sceneUV).z;
        if (rayZMin <= sceneZ && rayZMax >= sceneZ - screenEffectUniforms.ssrBias) {
            hitUV = sceneUV;
            numSteps = i;
            return true;
        }
        p += dP; q += dQ; k += dk;
    }
    return false;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ssrMain(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) {
        return;
    }
    int2 pixel = int2(dispatchId.xy);
    float2 uv = (float2(pixel) + 0.5) / float2(width, height);
    // Every pixel is written: this dispatch produces the whole image, not a
    // reflection layer somebody else composites.
    float4 scene = sSceneColor.SampleLevel(uv, 0.0);

    // Nothing was drawn here, so there is no surface to reflect off. Far depth
    // is the sky, which the resolve has already written into this pixel.
    float depth = sGBufDepth.SampleLevel(uv, 0.0).r;
    float4 mainTexSample = sGBufDiffuse.SampleLevel(uv, 0.0);
    float mirror = 1.0 - mainTexSample.a;
    if (depth >= 1.0 || mirror <= 0.0) {
        outImage[pixel] = scene;
        return;
    }

    float3 position = reconstructViewPos(uv);
    float3 eyeNormal = normalize(2.0 * sGBufEyeNormal.SampleLevel(uv, 0.0).xyz - 1.0);
    float3 reflection = reflect(normalize(position), eyeNormal);
    // Half a stride on a checkerboard, so that neighbouring rays do not all
    // test the same depths and step the same misses.
    float jitter = float((pixel.x + pixel.y) & 1) * 0.5;

    float2 hitUV;
    float numSteps;
    float3 reflectionColor = float3(0.0);
    float strength = 0.0;
    if (traceScreenSpaceRay(position, reflection, jitter, hitUV, numSteps)) {
        float2 hitNDC = hitUV * 2.0 - 1.0;
        float maxDim = min(1.0, max(abs(hitNDC.x), abs(hitNDC.y)));
        reflectionColor = sSceneColor.SampleLevel(hitUV, 0.0).rgb;
        // Three confidences, all of them about how much the screen actually
        // knows: a ray leaving toward the camera has nothing in front of it, a
        // ray that marched to its step limit never converged, and a hit near
        // the border reflects a surface that leaves the screen next frame.
        strength = 1.0 - clamp(reflection.z, 0.0, 1.0);
        strength *= 1.0 - numSteps / screenEffectUniforms.ssrMaxSteps;
        strength *= 1.0 - max(0.0, (maxDim - kEdgeFadeStart) / (1.0 - kEdgeFadeStart));
    }
    outImage[pixel] = float4(lerp(scene.rgb, reflectionColor, mirror * strength), scene.a);
}
