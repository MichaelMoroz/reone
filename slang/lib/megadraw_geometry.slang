// Shared declarations for raster passes over the canonical merged geometry.
// Both the G-buffer and shadow mega-draws use the same material gate so a
// punch-through surface cannot disagree about coverage between the two.

import lib.math;
import lib.scene_material;

static const uint kNoTexture = 0xffffffffu;
static const uint kMegaFeaturePremulAlpha = 1u << 12;
static const uint kMegaFeaturePunchThrough = 1u << 26;
static const float kMegaAlphaTestThreshold = 0.5;

struct MergedVertex {
    float3 position;
    float3 normal;
    float2 uv1;
    float2 uv2;
    float3 tangent;
    float3 bitangent;
    float3 tanSpaceNormal;
    float3 prevPosition;
    float2 pad;
    float4 color;
};

[[vk::binding(0, 2)]] StructuredBuffer<MergedVertex> mergedVertices;
[[vk::binding(1, 2)]] StructuredBuffer<uint> mergedMaterialIds;
[[vk::binding(2, 2)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 2)]] Sampler2D bindlessTextures[];
[[vk::binding(4, 2)]] Sampler2DArray bindlessTextureArrays[];

struct PushConstants {
    uint triangleBase;
    uint materialGated;
};
[[vk::push_constant]] PushConstants pushConstants;

float2 transformedUV(InstanceMaterial material, float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(material.uv0.x * h.x + material.uv1.x * h.y + material.uv2.x * h.z,
                  material.uv0.y * h.x + material.uv1.y * h.y + material.uv2.y * h.z);
}

// Keep the cutout thresholds in lockstep with candidateAlpha/commitsCoverage
// in tracing/trace.slang. The gated draw publishes CUTOUT COVERAGE ONLY:
// blended and additive surfaces contribute nothing until G8.
bool commitsTracedCoverage(InstanceMaterial material, float2 uv, float vertexAlpha) {
    if (pushConstants.materialGated == 0) return true;

    if ((material.featureMask & kMegaFeaturePunchThrough) == 0) return false;

    float alpha = 1.0;
    if (material.mainTex != kNoTexture) {
        float4 texel = bindlessTextures[NonUniformResourceIndex(material.mainTex)]
                           .SampleLevel(uv, 0.0);
        alpha = (material.featureMask & kMegaFeaturePremulAlpha) != 0
                    ? rgbToLuma(texel.rgb)
                    : texel.a;
        alpha *= vertexAlpha;
    }
    return alpha >= kMegaAlphaTestThreshold;
}
