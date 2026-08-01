/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The path tracer's GPU-resident scene: bindings, the per-instance material
// record, and push constants. Every other tracing module imports this one.
//
// Material assignment policy lives on the CPU: the TLAS admission in
// rayquery.cpp decides each instance's surface type and bakes the curated
// records and per-category calibration overrides into this struct's fields.
// The shader only evaluates; it never classifies.

public static const uint kPtNoTexture = 0xffffffffu;

/** Surface types, assigned CPU-side. Everything is PBR unless said otherwise. */
public static const uint kPtSurfacePbr = 0;              // parametrized PBR, optional emission
public static const uint kPtSurfaceUnlitTransparent = 1; // additive radiance, ray passes through
public static const uint kPtSurfaceUnlitEmissive = 2;    // sky and prelit: radiance, path ends

// featureMask bits the tracer consumes. Kept bit-compatible with the raster
// feature mask below bit 23; the tracer-only bits sit above.
public static const uint kPtMaskSky = 1u << 24;          // selects the sky dial over the emissive one
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
public static const uint kPtCategoryShift = 27;
public static const uint kPtCategoryMask = 0xFu;

/** Coverage below this never commits: the texel simply is not there. */
public static const float kPtAlphaTestEpsilon = 0.01;

public struct InstanceMaterial {
    public float4 selfIllumColor;
    public float4 diffuseColor;
    public float4 uv0;
    public float4 uv1;
    public float4 uv2;
    public uint mainTex;
    public uint normalMap;
    public uint lightmap;
    public uint bumpMapArray;
    public uint featureMask;
    public int bumpMapFrame;
    public float bumpMapScale;
    // Curated material operations; see rayquery.cpp for the encoding.
    public float4 curatedAlbedoMul;
    public float4 curatedRoughA;
    public float4 curatedRoughB;
    public float4 curatedMetalA;
    public float4 curatedMetalB;
    public float4 curatedEmission;
    /** kPtSurface*, decided at admission. */
    public uint surfaceType;
    // This occupies the scalar slot before the override vectors so category
    // tuning does not change the storage-buffer array stride.
    public float roughnessScale;
    // The per-category calibration overrides, baked at admission so the
    // shader never touches a category table: rgb + lerp weight flat-paints
    // the albedo; params are roughness override (negative disables),
    // emission scale, env-map strength scale.
    public float4 overrideColor;
    public float4 overrideParams;
};

[[vk::binding(0, 1)]] public RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] public RaytracingAccelerationStructure sceneTLAS;
[[vk::binding(2, 1)]] public StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 1)]] public RWStructuredBuffer<uint> traceStats;
public struct MergedVertex {
    public float3 position; public float3 normal; public float2 uv1; public float2 uv2;
    public float3 tangent; public float3 bitangent; public float3 tanSpaceNormal;
    public float3 prevPosition; public float2 pad; public float4 color;
};
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
    /** Scene-referred exposure applied before the tonemap. */
    public float exposure;
    public uint geometryBase0;
    public uint geometryBase1;
    /** A successful sky-room bake exists; otherwise geometry remains admitted. */
    public uint skyAvailable;
};
[[vk::push_constant]] public PtPushConstants pushConstants;

public static const uint kPtFlagStats = 1u;
public static const uint kPtDebugViewShift = 4;
public static const uint kPtDebugViewMask = 0xFu;
public static const uint kPtTonemapShift = 8;
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

public uint geometryBase(uint geometryIndex) { return geometryIndex == 0 ? pushConstants.geometryBase0 : pushConstants.geometryBase1; }
public uint materialIndexFor(uint geometryIndex, uint primitive) { return mergedMaterialIds[geometryBase(geometryIndex) + primitive]; }
public uint3 ptTriangleIndices(uint geometryIndex, uint primitive) {
    uint first = (geometryBase(geometryIndex) + primitive) * 3;
    return uint3(mergedIndices[first], mergedIndices[first + 1], mergedIndices[first + 2]);
}

public float2 ptTransformedUV(InstanceMaterial material, float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(material.uv0.x * h.x + material.uv1.x * h.y + material.uv2.x * h.z,
                  material.uv0.y * h.x + material.uv1.y * h.y + material.uv2.y * h.z);
}

public float3 ptSkyRadiance(float3 direction) {
    return skyCube.SampleLevel(normalize(direction), 0.0).rgb * pushConstants.skyIntensity;
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
