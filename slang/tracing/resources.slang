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

// The path tracer's GPU-resident scene: bindings, the per-instance material
// record, and push constants. Every other tracing module imports this one.
//
// Material assignment policy lives on the CPU: the TLAS admission in
// rayquery.cpp decides each instance's surface type and bakes the curated
// records and per-category calibration overrides into this struct's fields.
// The shader only evaluates; it never classifies.

import lib.scene_schema;
import lib.skycube;

public static const uint kPtNoTexture = 0xffffffffu;

/** Surface types, assigned CPU-side. Everything is PBR unless said otherwise. */
public static const uint kPtSurfacePbr = 0;              // parametrized PBR, optional emission
public static const uint kPtSurfaceUnlitTransparent = 1; // additive radiance, ray passes through
public static const uint kPtSurfaceUnlitEmissive = 2;    // sky and prelit: radiance, path ends

// featureMask bits the tracer consumes. Kept bit-compatible with the raster
// feature mask below bit 23; the tracer-only bits sit above.
public static const uint kPtMaskEnvMap = 1u << 1;
public static const uint kPtMaskSky = 1u << 24;          // selects the sky dial over the emissive one
/**
 * Sky-room scenery: rendered, but it casts no shadow.
 *
 * The sky room is the backdrop. Its shell is omitted from the scene outright,
 * and what remains inside it - Kashyyyk's trees, 001ebo16's asteroids and its
 * planet - is scenery standing in for distance. Letting it occlude puts the
 * backdrop's shadow on the playable area: on Kashyyyk the canopy closed over
 * the sun and the module lost its directional light entirely.
 *
 * Shading is untouched, deliberately. This says nothing about what the surface
 * looks like, only that shadow rays pass through it, so it cannot be folded
 * into kPtMaskSky - that bit picks the sky dial and forces unlit emissive.
 */
public static const uint kPtMaskBackdropNoShadow = 1u << 31;
// Continuous alpha coverage: shade this surface and transmit the remainder
// of the ray. Kept separate from punch-through because a smoke ramp is not a
// stochastic cutout mask.
public static const uint kPtMaskBlendedCoverage = 1u << 25;
// Bit-compatible with the raster feature mask. These textures carry colour
// already multiplied by coverage and NO usable alpha channel, so reading .a
// yields a meaningless constant - for KotOR's smoke that reads as opaque
// everywhere the puff is not, which renders the plume as its own negative.
// particles.slang:64-71 recovers coverage from luma; the tracer must do the
// same or it is shading a different surface than raster is.
public static const uint kPtMaskPremulAlpha = 1u << 12;
public static const uint kPtMaskPunchThrough = 1u << 26; // alpha-tested coverage in traversal
/**
 * Geometry the CPU synthesised rather than read from a model: grass, particle
 * plumes, lens-flare billboards. Set for all three by admission.
 *
 * On its own it is not a traversal decision - grass is procedural and is traced
 * as an ordinary cutout. Paired with the ABSENCE of kPtMaskPunchThrough it
 * names the procedural SPRITES, which is the population no ray sees.
 */
public static const uint kPtMaskProcedural = 1u << 16;
/**
 * Blended coverage that is scene geometry: a window, a force field, a leaf
 * card. Set by admission (kMaskTracedTransmissive in
 * src/libs/scene/render/admission.cpp), and by admission alone - the raster
 * passes never read it, because to them blended is blended.
 *
 * It exists because the two populations behind kPtMaskBlendedCoverage want
 * opposite treatment from a ray. A smoke puff is volume the marcher owns and
 * traversal is right to skip it. A pane of glass is a surface: the ray has to
 * see it, tint by what it passes through, and carry on.
 */
public static const uint kPtMaskTransmissive = 1u << 23;
/** No meaningful thickness: lit from both sides and passes light through. */
public static const uint kPtMaskThin = 1u << 15;
public static const uint kPtCategoryShift = 27;
public static const uint kPtCategoryMask = 0xFu;

/** Coverage below this never commits: the texel simply is not there. */
public static const float kPtAlphaTestEpsilon = 0.01;

[[vk::binding(0, 1)]] public RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] public RaytracingAccelerationStructure sceneTLAS;
[[vk::binding(2, 1)]] public StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 1)]] public RWStructuredBuffer<uint> traceStats;
[[vk::binding(4, 1)]] public StructuredBuffer<MergedVertex> mergedVertices;
[[vk::binding(5, 1)]] public StructuredBuffer<uint> mergedIndices;
[[vk::binding(6, 1)]] public StructuredBuffer<uint> mergedMaterialIds;
// This binding is deliberately both runtime-sized and non-uniformly indexed.
// The descriptor layout supplies UPDATE_AFTER_BIND and PARTIALLY_BOUND.
// The bindless arrays sit above every scalar binding because the variable
// descriptor count is only legal on the set's highest binding.
[[vk::binding(7, 1)]] public Sampler2D bindlessTextures[];
// Animated grayscale bump maps are Texture2DArray assets. They cannot share
// bindlessTextures: a Sampler2D descriptor has a different Vulkan view type.
[[vk::binding(8, 1)]] public Sampler2DArray bindlessTextureArrays[];
// The baked room background is an environment only for visibility rays. It is
// deliberately a single cube rather than a bindless resource: there is at
// most one selected sky room per module.
[[vk::binding(9, 1)]] public SamplerCube skyCube;

// The rasterized primary. Every mode draws this G-buffer before shading, so in
// this one the tracer reads its primary surface out of it rather than tracing a
// camera ray to find the same surface twice. Formats and encodings are
// VulkanGBuffer's, so these are the identical texels pbr_resolve.slang and
// retro_resolve.slang read - see tracing/primary.slang for the decode.
[[vk::binding(10, 1)]] public Sampler2D gbufDiffuse;
[[vk::binding(11, 1)]] public Sampler2D gbufEyeNormal;
[[vk::binding(12, 1)]] public Sampler2D gbufLightmap;
[[vk::binding(13, 1)]] public Sampler2D gbufSelfIllum;
[[vk::binding(14, 1)]] public Sampler2D gbufMotion;
[[vk::binding(15, 1)]] public Sampler2D gbufDepth;
// The triangle the fragment came from, in the same merged numbering
// ptTriangleIndices and mergedMaterialIds are addressed by. It is the one
// attachment that is expanded rather than decoded: primary.slang fetches its
// three vertices to build the geometric normal the stored normal cannot supply,
// and takes the material from mergedMaterialIds at the same index.
[[vk::binding(16, 1), vk::image_format("r32ui")]] public Sampler2D<uint> gbufTriangleId;
/**
 * Sixty-four independent 64x64 blue-noise tiles in an 8x8 grid - see
 * assets/README.md. Read with Load and integer coordinates, never sampled:
 * neighbouring tiles are unrelated fields, so any filtering across a tile edge
 * would blend two of them.
 */
[[vk::binding(17, 1)]] public Sampler2D blueNoise;
[[vk::binding(18, 1)]] public StructuredBuffer<float4> grassCardVertices;
[[vk::binding(19, 1)]] public StructuredBuffer<uint> grassCardIndices;
[[vk::binding(20, 1)]] public StructuredBuffer<GrassCardInstance> grassCardInstances;

/** The G-buffer's cleared triangle id: nothing was drawn, so this is the sky. */
public static const uint kPtNoTriangle = 0xffffffffu;

public struct PtPushConstants {
    public uint frameIndex;
    public uint samplesPerPixel;
    // Source intensities, not an image exposure: sky, other emitters, and the
    // baked radiance cache each get their own dial so the traced light balance
    // can be tuned live against raster without a shader rebuild.
    public float skyIntensity;
    public float emissiveIntensity;
    public float lightmapIntensity;
    public float directIntensity;
    // World-space offset of a secondary ray's origin along the geometric
    // normal. Too small self-intersects and speckles black; too large leaks
    // light through contact edges.
    public float rayOriginOffset;
    public float sunIntensity;
    public uint traceFlags;
    public uint bounceCount;
    // A point light's emitter radius as a fraction of its authored influence
    // radius. KotOR authored no emitter size and the influence radius is a
    // range, so the fraction is a dial. It is brightness-neutral by
    // construction - see ptSphereLight - and moves only penumbra width and
    // how bright a surface can get right against the lamp.
    public float emitterRadiusRatio;
    // The sun is not placed at a physical distance, so its angular radius
    // stays an authored number rather than falling out of a solid angle.
    public float sunAngularRadius;
    // Path regularisation and its floor; see GraphicsOptions.
    public float bounceRoughness;
    public float roughnessFloor;
    // Zero leaves indirect samples untouched.
    public float indirectClamp;
    /** Fraction of light a thin surface passes to its far side. */
    public float thinTransmission;
    /**
     * How far a transmitted ray bends at a transparent surface, 0 for not at
     * all. Artistic: Odyssey's glass is a flat card with no thickness and no
     * authored index, so any bend here is invention. It is a dial rather than a
     * constant for that reason, and at 0 the frame is the frame without it.
     */
    public float refraction;
    /** Scene-referred exposure applied before the tonemap. */
    public float exposure;
    public uint geometryBase0;
    public uint geometryBase1;
    /** A successful sky-room bake exists; otherwise geometry remains admitted. */
    public uint skyAvailable;
    /** Exponent authored surface colour is decoded with; see GraphicsOptions. */
    public float albedoGamma;
    /**
     * Backdrop imagery's own scale, beside the sky's.
     *
     * The painted distance: Taris' cityscape, Manaan's towers. They terminate
     * the path exactly as the sky does and for the same reason - nothing was
     * ever modelled behind them - so they are radiance, not reflectance, and
     * they belong on a dial of their own rather than on the emissive one that
     * grades lamps and screens.
     */
    public float backdropIntensity;
    /**
     * Exponent a texel is decoded with when it is radiance rather than
     * reflectance.
     *
     * Separate from albedoGamma because the two are calibrating different
     * things. Albedo gamma is a look control - Odyssey art was authored to be
     * multiplied by light unconverted, so the decode is a compromise and moving
     * it is how an area is made to read right. Emission has no such argument:
     * an authored glow colour is the colour that is emitted, and 2.2 is simply
     * the encoding it was stored in.
     *
     * They were one number, which meant grading a room's reflectance also
     * changed how bright its lamps and its skyline were - two effects with
     * nothing in common riding one slider.
     */
    public float emissiveGamma;
};
[[vk::push_constant]] public PtPushConstants pushConstants;

/**
 * Authored surface colour to linear.
 *
 * Separate from ptGammaToLinear, which is a fixed 2.2 and stays that way: it
 * is also the encode the post-process inverts, and the two must agree. This is
 * only the decode applied to reflectance, so it can move without putting a
 * global contrast shift through the frame.
 */
public float3 ptDecodeAlbedo(float3 rgb) {
    return pow(max(rgb, 0.0), float3(pushConstants.albedoGamma));
}

/** Authored radiance to linear - emission, sky and backdrop imagery. */
public float3 ptDecodeEmission(float3 rgb) {
    return pow(max(rgb, 0.0), float3(pushConstants.emissiveGamma));
}

public static const uint kPtFlagStats = 1u;
/**
 * Primary-vertex direct light takes its own channel to the resolve instead of
 * being accumulated into the denoised ones. Off routes it back through NRD,
 * which is what this replaced and what it is compared against.
 */
public static const uint kPtFlagDirectChannel = 4u;
/** Parity mode: the final image is only the shared unoccluded direct sum. */
public static const uint kPtFlagParityDirect = 1u << 12;
// Six bits, not four. Four held fifteen channels and the sixteenth silently
// wrapped to "off" - a debug view that quietly shows the ordinary picture is
// worse than one that fails.
public static const uint kPtDebugViewShift = 4;
public static const uint kPtDebugViewMask = 0x3Fu;
public static const uint kPtTonemapShift = 10;
public static const uint kPtTonemapMask = 0x3u;

public uint ptDebugView() {
    return (pushConstants.traceFlags >> kPtDebugViewShift) & kPtDebugViewMask;
}

public uint ptTonemapOperator() {
    return (pushConstants.traceFlags >> kPtTonemapShift) & kPtTonemapMask;
}

public bool ptStatsEnabled() {
    return (pushConstants.traceFlags & kPtFlagStats) != 0;
}

public bool ptParityDirectEnabled() {
    return (pushConstants.traceFlags & kPtFlagParityDirect) != 0;
}

public bool ptDirectChannelEnabled() {
    return (pushConstants.traceFlags & kPtFlagDirectChannel) != 0;
}

public uint geometryBase(uint geometryIndex) { return geometryIndex == 0 ? pushConstants.geometryBase0 : pushConstants.geometryBase1; }
public uint materialIndexFor(uint geometryIndex, uint primitive) { return mergedMaterialIds[geometryBase(geometryIndex) + primitive]; }
/**
 * The triangle's three merged vertex indices, from a triangle id.
 *
 * Traversal names a triangle as (geometry, primitive) and adds the base; the
 * raster pass writes the summed id straight into the G-buffer. Both end up
 * indexing this one array, which is what lets primary.slang recover a traced
 * hit record's geometry exactly.
 */
public uint3 ptTriangleIndicesById(uint triangleId) {
    uint first = triangleId * 3;
    return uint3(mergedIndices[first], mergedIndices[first + 1], mergedIndices[first + 2]);
}
public uint3 ptTriangleIndices(uint geometryIndex, uint primitive) {
    return ptTriangleIndicesById(geometryBase(geometryIndex) + primitive);
}

public float2 ptTransformedUV(InstanceMaterial material, float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(material.uv0.x * h.x + material.uv1.x * h.y + material.uv2.x * h.z,
                  material.uv0.y * h.x + material.uv1.y * h.y + material.uv2.y * h.z);
}

/**
 * This mode's binding and dial in front of the shared cube read. The body is
 * lib/skycube.slang's, so the raster resolves and the tracer cannot drift.
 */
public float3 ptSkyRadiance(float3 direction) {
    return skyRadiance(skyCube, direction, pushConstants.skyIntensity);
}

/** Narrow material-buffer fetch for alpha-tested traversal. */
public float2 ptTransformedUV(uint materialIndex, float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(instanceMaterials[materialIndex].uv0.x * h.x +
                      instanceMaterials[materialIndex].uv1.x * h.y +
                      instanceMaterials[materialIndex].uv2.x * h.z,
                  instanceMaterials[materialIndex].uv0.y * h.x +
                      instanceMaterials[materialIndex].uv1.y * h.y +
                      instanceMaterials[materialIndex].uv2.y * h.z);
}
