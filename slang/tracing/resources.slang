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
// Material classification is CPU-side; the shader only evaluates.

import lib.scene_schema;
import lib.skycube;

/** Surface types, assigned CPU-side. Everything is PBR unless said otherwise. */
public static const uint kPtSurfacePbr = 0;              // parametrized PBR, optional emission
public static const uint kPtSurfaceUnlitTransparent = 1; // additive radiance, ray passes through
public static const uint kPtSurfaceUnlitEmissive = 2;    // sky and prelit: radiance, path ends

/** Coverage below this never commits: the texel simply is not there. */
public static const float kPtAlphaTestEpsilon = 0.01;

[[vk::binding(0, 1)]] public RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] public RaytracingAccelerationStructure sceneTLAS;
[[vk::binding(2, 1)]] public StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 1)]] public RWStructuredBuffer<uint> traceStats;
[[vk::binding(4, 1)]] public StructuredBuffer<MergedVertex> mergedVertices;
[[vk::binding(5, 1)]] public StructuredBuffer<uint> mergedIndices;
[[vk::binding(6, 1)]] public StructuredBuffer<uint> mergedMaterialIds;
// Runtime-sized and non-uniformly indexed; the layout supplies UPDATE_AFTER_BIND
// and PARTIALLY_BOUND. Variable descriptor count is legal only on the set's
// highest binding, so the bindless arrays sit above every scalar one.
[[vk::binding(7, 1)]] public Sampler2D bindlessTextures[];
// Animated bump maps are Texture2DArray assets: a different Vulkan view type.
[[vk::binding(8, 1)]] public Sampler2DArray bindlessTextureArrays[];
// One cube, not bindless: at most one selected sky room per module.
[[vk::binding(9, 1)]] public SamplerCube skyCube;

// The rasterized primary the tracer starts from; VulkanGBuffer's formats and
// encodings, decoded in tracing/primary.slang.
[[vk::binding(10, 1)]] public Sampler2D gbufDiffuse;
[[vk::binding(11, 1)]] public Sampler2D gbufEyeNormal;
[[vk::binding(12, 1)]] public Sampler2D gbufLightmap;
[[vk::binding(13, 1)]] public Sampler2D gbufSelfIllum;
[[vk::binding(14, 1)]] public Sampler2D gbufMotion;
[[vk::binding(15, 1)]] public Sampler2D gbufDepth;
// The fragment's triangle in merged numbering; primary.slang rebuilds the
// geometric normal and material from it.
[[vk::binding(16, 1), vk::image_format("r32ui")]] public Sampler2D<uint> gbufTriangleId;
/** 64 independent 64x64 blue-noise tiles in an 8x8 grid; Load, never sample across a tile edge. */
[[vk::binding(17, 1)]] public Sampler2D blueNoise;
[[vk::binding(18, 1)]] public StructuredBuffer<float4> grassCardVertices;
[[vk::binding(19, 1)]] public StructuredBuffer<uint> grassCardIndices;
[[vk::binding(20, 1)]] public StructuredBuffer<GrassCardInstance> grassCardInstances;


public struct PtPushConstants {
    public uint frameIndex;
    public uint samplesPerPixel;
    // Source intensities, one dial each, tuned live against raster.
    public float skyIntensity;
    public float emissiveIntensity;
    public float lightmapIntensity;
    public float directIntensity;
    // Secondary-ray origin offset along the geometric normal.
    public float rayOriginOffset;
    public float sunIntensity;
    public uint traceFlags;
    public uint bounceCount;
    // Emitter radius as a fraction of the authored influence radius; brightness-neutral.
    public float emitterRadiusRatio;
    // Authored, since the sun sits at no physical distance.
    public float sunAngularRadius;
    // Path regularisation and its floor; see GraphicsOptions.
    public float bounceRoughness;
    public float roughnessFloor;
    // Zero leaves indirect samples untouched.
    public float indirectClamp;
    /** Fraction of light a thin surface passes to its far side. */
    public float thinTransmission;
    /** Bend of a transmitted ray, 0 for none. Artistic: Odyssey's glass has no thickness. */
    public float refraction;
    /** Scene-referred exposure applied before the tonemap. */
    public float exposure;
    public uint geometryBase0;
    public uint geometryBase1;
    /** A successful sky-room bake exists; otherwise geometry remains admitted. */
    public uint skyAvailable;
    /** Exponent authored surface colour is decoded with; see GraphicsOptions. */
    public float albedoGamma;
    /** Painted distance (a skyline) is radiance that ends the path, on its own dial. */
    public float backdropIntensity;
    /** Decode exponent for radiance texels; albedoGamma is a look control, this is not. */
    public float emissiveGamma;
    /** Next-event draws per shading vertex; one is the classic estimator. */
    public uint neeSamples;
};
[[vk::push_constant]] public PtPushConstants pushConstants;

/** Authored reflectance to linear. Separate from the fixed 2.2 the post-process inverts. */
public float3 ptDecodeAlbedo(float3 rgb) {
    return pow(max(rgb, 0.0), float3(pushConstants.albedoGamma));
}

/** Authored radiance to linear - emission, sky and backdrop imagery. */
public float3 ptDecodeEmission(float3 rgb) {
    return pow(max(rgb, 0.0), float3(pushConstants.emissiveGamma));
}

public static const uint kPtFlagStats = 1u;
/** Primary-vertex direct light takes its own channel instead of going through NRD. */
public static const uint kPtFlagDirectChannel = 4u;
/** Parity mode: the final image is only the shared unoccluded direct sum. */
public static const uint kPtFlagParityDirect = 1u << 12;
/** Fog master switch; surfaces without the fog feature are never fogged regardless. */
public static const uint kPtFlagFog = 1u << 13;
/** Next-event estimation; off, analytic lights are found by no ray - a diagnostic. */
public static const uint kPtFlagNee = 1u << 14;
/** Grass is visible to scatter rays. */
public static const uint kPtFlagGrassScatter = 1u << 15;
/** Grass is visible to shadow rays, traced once per light per vertex. */
public static const uint kPtFlagGrassShadows = 1u << 16;
/** Six bits: four wrapped the sixteenth debug view silently to "off". */
public static const uint kPtDebugViewShift = 4;
public static const uint kPtDebugViewMask = 0x3Fu;
public static const uint kPtTonemapShift = 10;
public static const uint kPtTonemapMask = 0x3u;

public bool ptFlag(uint bit) { return (pushConstants.traceFlags & bit) != 0; }
public uint ptDebugView() { return (pushConstants.traceFlags >> kPtDebugViewShift) & kPtDebugViewMask; }
public uint ptTonemapOperator() { return (pushConstants.traceFlags >> kPtTonemapShift) & kPtTonemapMask; }

public uint geometryBase(uint geometryIndex) { return geometryIndex == 0 ? pushConstants.geometryBase0 : pushConstants.geometryBase1; }
public uint materialIndexFor(uint geometryIndex, uint primitive) { return mergedMaterialIds[geometryBase(geometryIndex) + primitive]; }
/** The triangle's merged vertex indices; the G-buffer stores the same summed id. */
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

/** This mode's binding and dial in front of lib/skycube.slang's shared read. */
public float3 ptSkyRadiance(float3 direction) {
    return skyRadiance(skyCube, direction, pushConstants.skyIntensity);
}

public float2 ptTransformedUV(uint materialIndex, float2 uv) {
    return ptTransformedUV(instanceMaterials[materialIndex], uv);
}
