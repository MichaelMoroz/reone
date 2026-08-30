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
// Canonical GPU-scene storage schema.  These declarations deliberately have
// no bindings: each pass owns its descriptor set, while every pass shares the
// byte layout.  C++ mirrors live in graphics/gpuscene.h and are checked from
// Slang reflection when the Vulkan renderer starts.
module scene_schema;

/**
 * One deduplicated material record. Admission bakes every authored texture
 * reference and every per-category calibration override into these fields, so
 * the shader only ever evaluates; it never classifies.
 */
public struct InstanceMaterial {
    public float4 selfIllumColor;
    public float4 diffuseColor;
    public float4 uv0;
    public float4 uv1;
    public float4 uv2;
    // Bindless descriptor indices; kNoTexture means the slot is unused.
    public uint mainTex;
    public uint normalMap;
    public uint lightmap;
    public uint bumpMapArray;
    public uint featureMask;
    public int bumpMapFrame;
    public float bumpMapScale;
    public uint envMap;
    // Curated material operations; see rayquery.cpp for the encoding.
    public float4 curatedAlbedoMul;
    public float4 curatedRoughA;
    public float4 curatedRoughB;
    public float4 curatedMetalA;
    public float4 curatedMetalB;
    public float4 curatedEmission;
    /** kPtSurface*, decided at admission. */
    public uint surfaceType;
    /** The category's metalness, or negative for curated. */
    public float overrideMetallic;
    public uint envMapCube;
    public float waterAlpha;
    // The category's PBR properties for this surface's class, baked at
    // admission: rgb + lerp weight flat-paints the albedo; the texel's alpha
    // lerps roughness and specular weight (r0, r1, s0, s1) and f0 (f0_0, f0_1)
    // between their alpha-0 and alpha-1 values. emission is the grade:
    // intensity, decode exponent.
    public float4 overrideColor;
    public float4 overrideParams;
    public float4 overrideF0;
    public float4 emission;
    public float4 ambientColor;
    public int envMapDerivedLayer;
    /** Authored cutout threshold, or negative where the texture carries none. */
    public float alphaTest;
};

/** Three row vectors encode a float3x4 exactly as scene_resolve.slang reads it. */
public struct Matrix3x4 {
    public float4 row0;
    public float4 row1;
    public float4 row2;
};

/** The canonical world-space vertex every consumer reads after the merge. */
public struct MergedVertex {
    public float3 position;
    public float3 normal;
    public float2 uv1;
    public float2 uv2;
    public float3 tangent;
    public float3 bitangent;
    public float3 tanSpaceNormal;
    public float3 prevPosition;
    public float2 pad;
    public float4 color;
};

/** One admitted object, consumed by the merge dispatch and nothing else. */
public struct SceneObject {
    public float4x4 transform;
    public float4x4 prevTransform;
    public float4x4 transformInv;
    // Element offsets: floats in sourceVertices and uints in sourceIndices.
    // Attribute offsets below remain byte offsets inside each vertex record.
    // A stride of 0 marks grass and 1 a procedural quad, in which case the two
    // offsets above name a grass range instead of source geometry.
    public uint srcVertexOffset;
    public uint srcIndexOffset;
    public uint srcVertexStride;
    public int offPosition;
    public int offNormals;
    public int offUV1;
    public int offUV2;
    public int offTanSpace;
    public int offBoneIndices;
    public int offBoneWeights;
    public uint vertexCount;
    public uint triangleCount;
    public uint dstVertexBase;
    public uint dstTriangleBase;
    public uint dstCardBase;
    public uint cardCount;
    public uint geometryIndex;
    public uint boneBase;
    public uint boneCount;
    public uint materialIndex;
    public uint danglyBase;
    public float4 saberDisplacement;
};

public struct GrassCardInstance {
    public float4 row0;
    public float4 row1;
    public float4 row2;
    public float4 prevRow0;
    public float4 prevRow1;
    public float4 prevRow2;
    public float4 lightmapVariantCulled;
    public uint4 materialIndex;
};

public struct ProceduralQuad {
    public float4 positionVariant;
    public float4 right;
    public float4 up;
    public float4 uvOffsetScale;
    public float2 lightmapUV;
    public float2 pad;
    public float4 color;
};

/**
 * One authored grass face, already in world space. UV components are packed
 * into otherwise unused fourth lanes so the merge shader reads one
 * cache-line-aligned record without a second source stream.
 */
public struct GrassFace {
    public float4 vertex0Uv0x;
    public float4 vertex1Uv0y;
    public float4 vertex2Uv1x;
    public float4 uv1yUv2QuadSize;
    public float4 probabilities;
    public float4 boundsMin;
    public float4 boundsMax;
    public uint4 faceBudgetMaterialVariants;
};

// InstanceMaterial::featureMask bits, one table for every consumer. Bits below
// 16 are the per-object feature mask the raster passes always carried; the
// rest were added by admission for the tracer and the merged draws.
public static const uint kMaterialLightmap = 1u << 0;
public static const uint kMaterialEnvMap = 1u << 1;
public static const uint kMaterialNormalMap = 1u << 2;
public static const uint kMaterialBumpMap = 1u << 3;
public static const uint kMaterialShadows = 1u << 7;
public static const uint kMaterialWater = 1u << 8;
public static const uint kMaterialFog = 1u << 9;
/** Colour premultiplied by coverage, no usable alpha: coverage is recovered from luma. */
public static const uint kMaterialPremulAlpha = 1u << 12;
public static const uint kMaterialEnvMapCube = 1u << 13;
public static const uint kMaterialStatic = 1u << 14;
/** No thickness: lit from both sides and passes light through. */
public static const uint kMaterialThin = 1u << 15;
/** CPU-synthesised geometry; without kMaterialPunchThrough it names the sprites no ray sees. */
public static const uint kMaterialProcedural = 1u << 16;
/** Blended coverage that is scene geometry (glass, leaf card): a ray sees it, tints, carries on. */
public static const uint kMaterialTransmissive = 1u << 23;
/** Selects the sky dial over the emissive one. */
public static const uint kMaterialSky = 1u << 24;
/** Continuous alpha coverage, distinct from a punch-through cutout mask. */
public static const uint kMaterialBlendedCoverage = 1u << 25;
/** Alpha-tested coverage, in traversal and in the raster gate alike. */
public static const uint kMaterialPunchThrough = 1u << 26;
public static const uint kMaterialCategoryShift = 27;
public static const uint kMaterialCategoryMask = 0xFu;
/** Sky-room scenery: shaded normally, but shadow rays pass through it. */
public static const uint kMaterialBackdropNoShadow = 1u << 31;

public static const uint kNoTexture = 0xffffffffu;
/** The G-buffer's cleared triangle id: nothing was drawn, so this is the sky. */
public static const uint kNoTriangle = 0xffffffffu;

/** Cutout coverage is binary; a texture may author its own threshold. */
public static const float kMaterialAlphaTestThreshold = 0.5;

// TLAS instance mask bits, ANDed with a ray's inclusion mask; a culled instance carries 0.
public static const uint kPtInstanceGeometry = 0x01u;
public static const uint kPtInstanceGrass = 0x02u;

/** Per-frame prefix-sum entry selecting a persistent grass face. */
public struct GrassRange {
    public uint faceIndex;
    public uint clusterOffset;
    public uint clusterCount;
    public uint pad;
};

/** The authored alpha-test threshold, or the shared default. Raster gate and tracer resolve it here. */
public float alphaTestThreshold(InstanceMaterial material) {
    return material.alphaTest >= 0.0 ? material.alphaTest : kMaterialAlphaTestThreshold;
}
