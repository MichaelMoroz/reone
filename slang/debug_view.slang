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
// The debug channel views, common to every render mode.
//
// These used to live inside the path tracer and only the traced mode honoured
// them, which made the one tool for asking "what is this pixel" unavailable in
// the two modes it would most often be used to compare against. Almost all of
// the channels are G-buffer quantities, and every mode rasterizes the same
// G-buffer before it shades anything, so almost all of them are answerable
// anywhere. This dispatch answers them, from the G-buffer, in whichever mode is
// running, and overwrites the shaded image with the result.
//
// The radiance channels - demodulated diffuse, specular, noise-free and direct
// diffuse - are read from the shared channel images, which any provider fills:
// the tracer's kernel, or PBR's channels pass. One channel, one view, whichever
// mode delivered the light. Retro fills no channels, and for it this pass
// paints an unmistakable "not available" card - see unavailableCard - as it
// does for the denoiser-only channels in any raster mode. It never substitutes
// a different channel, because a diagnostic that quietly shows you something
// else is worse than one that shows you nothing.
//
// The output is display-referred and final. A debug view is not a picture, so
// the frame plan skips the anti-aliasing, grade and sharpen stages behind it in
// every mode, for the same reason: a filter or a tone curve would change the
// values the view exists to let you read off the screen.

import uniforms;
import lib.gbuf;
import lib.material_ops;
import lib.math;
import lib.scene_schema;
// For ptDisplayTransform: the tracer's channel views encode through it
// (path_trace.slang), and one quantity in two colour spaces is not a
// comparison. Colour maths only; no tracing entry points are referenced.
import tracing.brdf;

[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(20, 1)]] Sampler2D sGBufMotion;
[[vk::binding(21, 1), vk::image_format("r32ui")]] Sampler2D<uint> sGBufTriangleId;
// The shared channel images, for the radiance views; unbound (and never
// sampled, see the channels-absent bit) in retro.
[[vk::binding(24, 1)]] Sampler2D sChannelDiffuse;
[[vk::binding(25, 1)]] Sampler2D sChannelSpecular;
[[vk::binding(26, 1)]] Sampler2D sChannelNoiseFree;
[[vk::binding(27, 1)]] Sampler2D sChannelDirect;
// The triangle-to-material table. The material id this view colours is now
// derived rather than stored, so the channel reports the same number the
// resolves shade with, by the same lookup.
[[vk::binding(1, 2)]] StructuredBuffer<uint> mergedMaterialIds;
[[vk::binding(2, 2)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(0, 3)]] RWTexture2D<float4> outImage;

/** Mirrors DebugViewPushConstants in scenepipeline.cpp. */
struct DebugViewPushConstants {
    /** The channel in the low byte; bit 8 says the channel images are absent
        (retro), so the radiance views paint the card instead of sampling a
        stand-in texture and reporting confident fiction. */
    uint view;
    /** The floor the shading used; the roughness view must show that number. */
    float roughnessFloor;
    /** Mirrors the tracer's channel encode; see the write at the entry point. */
    float exposure;
    uint tonemap;
};
[[vk::push_constant]] DebugViewPushConstants pushConstants;

static const uint kViewChannelsAbsent = 1u << 8;

// The common channel numbering. Gaps are the traced-only channels, whose
// numbers stay reserved so a saved --debugview value keeps its meaning.
static const uint kDebugOff = 0;
static const uint kDebugCategories = 1;
static const uint kDebugEmissive = 2;
static const uint kDebugNormals = 3;
static const uint kDebugRoughness = 4;
static const uint kDebugMetallic = 5;
static const uint kDebugLightmap = 6;
static const uint kDebugAlbedo = 7;
static const uint kDebugTracedDiffuse = 8;   // channel image 0
static const uint kDebugTracedSpecular = 9;  // channel image 1
static const uint kDebugDepth = 10;
static const uint kDebugTracedNoiseFree = 11; // channel image 5
static const uint kDebugMotion = 12;
static const uint kDebugMaterialId = 13;
static const uint kDebugFeatures = 14;
static const uint kDebugDirectShadow = 15;     // channel image 14
static const uint kDebugPenumbra = 16;         // tracer only
// Produced after denoising rather than before it, so that each component can be
// judged on what the denoiser did to it rather than on what it was handed.
static const uint kDebugDenoisedDiffuse = 17;  // tracer only
static const uint kDebugDenoisedSpecular = 18; // tracer only
static const uint kDebugFilteredDirect = 19;   // tracer only
// Channel 20 is gone: it re-derived PBR's shaded direct diffuse so it could be
// compared with the tracer's channel 15. PBR writes its direct diffuse into
// that same channel image now, so 15 IS both modes' answer - one channel, one
// view, each mode's own visibility - and a second derivation could only drift
// from what the provider actually delivered.


// Mirrors the category shift the admission path packs into every material
// record; see scene/render/admission.cpp.
static const uint kCategoryShift = 27;
static const uint kCategoryMask = 0xFu;
static const uint kSurfacePbr = 0;

static const float3 kCategoryPalette[9] = {
    float3(0.5, 0.5, 0.5),  // GUI - never admitted
    float3(0.3, 0.5, 0.9),  // Room
    float3(0.9, 0.3, 0.3),  // Creature
    float3(0.3, 0.9, 0.4),  // Placeable
    float3(0.9, 0.2, 0.9),  // Door
    float3(0.9, 0.8, 0.2),  // Equipment
    float3(0.5, 0.5, 0.5),  // Projectile - never admitted
    float3(0.5, 0.5, 0.5),  // Camera - never admitted
    float3(0.9, 0.5, 0.1),  // no model root
};

/** The sky, in every categorical view: no surface, one colour. */
static const float3 kSkyColor = float3(0.1, 0.8, 0.9);


// Must agree with isTracedOnlyDebugView in scenepipeline.h; see the note there.
bool tracedOnly(uint view) {
    return view == kDebugPenumbra || view == kDebugDenoisedDiffuse ||
           view == kDebugDenoisedSpecular || view == kDebugFilteredDirect;
}

/** The views answered from the shared channel images. */
bool channelView(uint view) {
    return view == kDebugTracedDiffuse || view == kDebugTracedSpecular ||
           view == kDebugTracedNoiseFree || view == kDebugDirectShadow;
}

/**
 * "This channel does not exist in this mode", drawn so that it cannot be
 * mistaken for data.
 *
 * No font is available to a compute dispatch, so the statement is made
 * graphically: magenta-on-charcoal hazard diagonals, which no channel this pass
 * can render ever produces. Black would have been indistinguishable from a
 * channel that is legitimately zero everywhere, which is exactly the reading
 * error worth preventing.
 */
float3 unavailableCard(int2 pixel) {
    float stripe = frac(float(pixel.x + pixel.y) * (1.0 / 48.0));
    return stripe < 0.5 ? float3(0.55, 0.0, 0.55) : float3(0.06, 0.06, 0.08);
}

/** Distinct, stable colour per material record; adjacent ids differ strongly. */
float3 materialIdColor(uint materialId) {
    uint h = materialId * 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    return float3(float((h >> 0) & 0xffu), float((h >> 8) & 0xffu),
                  float((h >> 16) & 0xffu)) * (1.0 / 255.0);
}

/** View-space position, exactly as both raster resolves reconstruct it. */
float3 reconstructViewPos(float2 uv, float depth) {
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth);
    float4 viewPos = mul(globalUniforms.projectionInv, float4(ndc, 1.0));
    return viewPos.xyz / viewPos.w;
}

float3 channelColor(float2 uv, int2 pixel) {
    uint view = pushConstants.view & 0xffu;
    // The radiance views, straight off the channel images, ahead of the sky
    // branch: the channels carry their own value at the sky (zero radiance, or
    // the sky's noise-free), exactly as the tracer's kernel views showed it.
    switch (view) {
    case kDebugTracedDiffuse:
        return max(sChannelDiffuse.SampleLevel(uv, 0.0).rgb, 0.0);
    case kDebugTracedSpecular:
        return max(sChannelSpecular.SampleLevel(uv, 0.0).rgb, 0.0);
    case kDebugTracedNoiseFree:
        return max(sChannelNoiseFree.SampleLevel(uv, 0.0).rgb, 0.0);
    case kDebugDirectShadow:
        // Exposed with 1 - exp(-x), as the tracer's kernel exposed it: an
        // unbounded radiance read out of an 8-bit capture clips raw, and a
        // clipped frame has no shadows in it.
        return float3(1.0) - exp(-max(sChannelDirect.SampleLevel(uv, 0.0).rgb, 0.0));
    default:
        break;
    }
    uint triangleId = sGBufTriangleId.Load(int3(pixel, 0)).r;
    if (triangleId == kNoTriangle) {
        // Nothing was drawn: the sky. Every categorical view says so with one
        // colour; the numeric ones report the value they would report.
        switch (view) {
        case kDebugDepth:
            return float3(1.0);
        case kDebugMotion:
            return float3(0.5, 0.5, 0.0);
        case kDebugNormals:
            return float3(0.5);
        case kDebugRoughness:
        case kDebugMetallic:
        case kDebugAlbedo:
            return float3(0.0);
        default:
            return kSkyColor;
        }
    }

    uint materialId = mergedMaterialIds[triangleId];
    InstanceMaterial material = instanceMaterials[materialId];
    float4 diffuseSample = sGBufDiffuse.SampleLevel(uv, 0.0);
    float4 lightmapSample = sGBufLightmap.SampleLevel(uv, 0.0);
    float3 eyeNormal = normalize(2.0 * sGBufEyeNormal.SampleLevel(uv, 0.0).xyz - 1.0);
    float3 normal = normalize(mul(globalUniforms.viewInv, float4(eyeNormal, 0.0)).xyz);

    bool envmapped, hasLightmap, staticObject, shadowsEnabled, fogEnabled, thinSurface;
    unpackGeometryFeatures(lightmapSample.a, envmapped, hasLightmap, staticObject,
                           shadowsEnabled, fogEnabled, thinSurface);

    // The shape cue: only the categorical views take it. A data channel
    // multiplied by a lighting term is a lit render, and a wrong value in one
    // becomes indistinguishable from a steep surface.
    float shape = 0.35 + 0.65 * abs(eyeNormal.z);

    switch (view) {
    case kDebugCategories: {
        uint category = (material.featureMask >> kCategoryShift) & kCategoryMask;
        float3 color = kCategoryPalette[min(category, 8u)] * shape;
        if ((material.featureMask & kMaterialSky) != 0) color = kSkyColor;
        return color;
    }
    case kDebugEmissive: {
        float3 emission = gammaToLinear(sGBufSelfIllum.SampleLevel(uv, 0.0).rgb);
        float emissiveLuma = dot(emission, float3(0.2126, 0.7152, 0.0722));
        return emissiveLuma > 0.001
                   ? float3(1.0, 0.25 + 0.75 * saturate(emissiveLuma), 0.0)
                   : dot(gammaToLinear(diffuseSample.rgb), float3(0.333, 0.333, 0.333)) *
                         shape * float3(0.35);
    }
    case kDebugNormals:
        return normal * 0.5 + 0.5;
    // Raw, deliberately: these are numbers to be read off the screen.
    case kDebugRoughness: {
        // The same stand-in and the same curation the shading applies: Odyssey
        // assets carry no roughness channel, so diffuse alpha is it.
        float3 albedo = gammaToLinear(diffuseSample.rgb) * material.curatedAlbedoMul.rgb;
        // The live floor, not a hardcoded 0.2. This channel exists to show the
        // number the shading used, and it was showing a different one.
        float roughness = curatedChannel(material.curatedRoughA, material.curatedRoughB,
                                         albedo,
                                         clamp(diffuseSample.a, pushConstants.roughnessFloor, 1.0));
        if (material.overrideParams.x >= 0.0) roughness = material.overrideParams.x;
        // Saturate, matching lib/material_ops.slang - this view has to show the
        // number the shading used, and the floor is not applied here.
        return float3(saturate(roughness * material.roughnessScale));
    }
    case kDebugMetallic: {
        // Odyssey materials are all dielectric, so anything non-zero here is
        // curation plus the per-category dial - which is the point of looking.
        float3 albedo = gammaToLinear(diffuseSample.rgb) * material.curatedAlbedoMul.rgb;
        float metallic = curatedChannel(material.curatedMetalA, material.curatedMetalB,
                                        albedo, 0.0);
        return float3(saturate(metallic * material.overrideParams.w));
    }
    case kDebugLightmap:
        return hasLightmap ? lightmapSample.rgb : float3(0.9, 0.0, 0.9) * shape * 0.4;
    case kDebugDepth: {
        float depth = sGBufDepth.SampleLevel(uv, 0.0).r;
        // Positive linear view distance on a fixed 100-unit ramp, so the same
        // surface reads the same grey in every mode and between two captures.
        return float3(saturate(abs(reconstructViewPos(uv, depth).z) / 100.0));
    }
    case kDebugMotion: {
        // xy around mid-grey, magnitude in blue: statics read flat, movers
        // light up. The vector is the G-buffer's own UV-space displacement.
        float2 motion = sGBufMotion.SampleLevel(uv, 0.0).xy;
        return float3(saturate(motion.x * 40.0 + 0.5), saturate(motion.y * 40.0 + 0.5),
                      saturate(length(motion) * 40.0));
    }
    case kDebugMaterialId:
        return materialIdColor(materialId);
    case kDebugFeatures: {
        // Three bits in the primaries and two as modifiers, so all five are
        // legible at once: red env-mapped, green lightmapped, blue static;
        // half-brightness when the surface takes no shadow; a cyan floor when
        // it is fogged.
        float3 color = float3(envmapped ? 1.0 : 0.0, hasLightmap ? 1.0 : 0.0,
                              staticObject ? 1.0 : 0.0);
        color *= shadowsEnabled ? 1.0 : 0.5;
        if (fogEnabled) color = max(color, float3(0.0, 0.25, 0.25));
        return color;
    }
    default:
        // Albedo, and the fallback for a value this pass does not know. The
        // G-buffer already stores the authored gamma-encoded texel, which is
        // what an albedo view wants to show.
        return diffuseSample.rgb;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void debugViewMain(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) {
        return;
    }
    int2 pixel = int2(dispatchId.xy);
    float2 uv = (float2(pixel) + 0.5) / float2(width, height);
    uint view = pushConstants.view & 0xffu;
    bool noChannels = (pushConstants.view & kViewChannelsAbsent) != 0;
    float3 color = tracedOnly(view) || (noChannels && channelView(view))
                       ? unavailableCard(pixel)
                       : channelColor(uv, pixel);
    // The tracer's channel views leave finishPixel through ptDisplayTransform
    // (path_trace.slang); this pass runs after post-processing and overwrites
    // its output, so it must encode for itself or its channels land raw while
    // the tracer's land display-encoded - one quantity in two colour spaces,
    // measured as a pi-shaped error before the encoding was checked.
    outImage[pixel] = float4(
        ptDisplayTransform(max(color, 0.0), pushConstants.exposure, pushConstants.tonemap),
        1.0);
}
