/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The path tracer's GPU-resident scene: bindings, the per-instance material
// record, push constants, and the raw-address vertex fetch helpers. Every
// other tracing module imports this one.
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
    public uint64_t vertexAddress;
    public uint64_t indexAddress;
    public uint vertexStride;
    public int offPosition;
    public int offNormals;
    public int offUV1;
    public int offUV2;
    public int offTanSpace;
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
    // Previous world transform, explicit columns; motion vectors only.
    public float4 prevTransform0;
    public float4 prevTransform1;
    public float4 prevTransform2;
    public float4 prevTransform3;
    // Previous-pose skinned positions, float4 per vertex; 0 when not skinned.
    public uint64_t prevPositionsAddress;
    /** kPtSurface*, decided at admission. */
    public uint surfaceType;
    public uint surfacePad;
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
// This binding is deliberately both runtime-sized and non-uniformly indexed.
// The descriptor layout supplies UPDATE_AFTER_BIND and PARTIALLY_BOUND.
// The bindless arrays sit above every scalar binding because the variable
// descriptor count is only legal on the set's highest binding.
[[vk::binding(5, 1)]] public Sampler2D bindlessTextures[];
// Animated grayscale bump maps are Texture2DArray assets. They cannot share
// bindlessTextures: a Sampler2D descriptor has a different Vulkan view type.
[[vk::binding(6, 1)]] public Sampler2DArray bindlessTextureArrays[];

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
    // Angular radius of light sources, radians. Never zero - a light source
    // is never a point: points wide for soft penumbras, the sun fairly sharp.
    public float pointAngularRadius;
    public float sunAngularRadius;
    /** Scene-referred exposure applied before the tonemap. */
    public float exposure;
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

// Raw-address vertex fetches. This Slang version exposes physical storage
// through ConstBufferPointer rather than DXC's vk::RawBufferLoad; the
// explicit 4-byte alignment is the same raw-address operation.

public float ptLoadFloat(uint64_t address) {
    ConstBufferPointer<uint, 4> pointer = ConstBufferPointer<uint, 4>(address);
    return asfloat(pointer[0]);
}

public float2 ptLoadFloat2(uint64_t address) {
    return float2(ptLoadFloat(address), ptLoadFloat(address + 4));
}

public float3 ptLoadFloat3(uint64_t address) {
    return float3(ptLoadFloat(address), ptLoadFloat(address + 4), ptLoadFloat(address + 8));
}

public uint ptLoadIndex16(uint64_t indexAddress, uint index) {
    uint64_t address = indexAddress + uint64_t(index) * 2;
    ConstBufferPointer<uint, 4> pointer = ConstBufferPointer<uint, 4>(address & ~uint64_t(3));
    uint packed = pointer[0];
    return (packed >> (uint(address & 2) * 8)) & 0xffffu;
}

public uint3 ptTriangleIndices(InstanceMaterial material, uint primitive) {
    uint first = primitive * 3;
    return uint3(ptLoadIndex16(material.indexAddress, first),
                 ptLoadIndex16(material.indexAddress, first + 1),
                 ptLoadIndex16(material.indexAddress, first + 2));
}

public uint64_t ptAttributeAddress(InstanceMaterial material, uint vertex, int offset) {
    return material.vertexAddress + uint64_t(vertex) * uint64_t(material.vertexStride) + uint64_t(offset);
}

public float2 ptInterpolateFloat2(InstanceMaterial material, uint3 indices, int offset, float3 weights) {
    return ptLoadFloat2(ptAttributeAddress(material, indices.x, offset)) * weights.x +
           ptLoadFloat2(ptAttributeAddress(material, indices.y, offset)) * weights.y +
           ptLoadFloat2(ptAttributeAddress(material, indices.z, offset)) * weights.z;
}

public float3 ptInterpolateFloat3(InstanceMaterial material, uint3 indices, int offset, float3 weights) {
    return ptLoadFloat3(ptAttributeAddress(material, indices.x, offset)) * weights.x +
           ptLoadFloat3(ptAttributeAddress(material, indices.y, offset)) * weights.y +
           ptLoadFloat3(ptAttributeAddress(material, indices.z, offset)) * weights.z;
}

public float2 ptTransformedUV(InstanceMaterial material, float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(material.uv0.x * h.x + material.uv1.x * h.y + material.uv2.x * h.z,
                  material.uv0.y * h.x + material.uv1.y * h.y + material.uv2.y * h.z);
}
