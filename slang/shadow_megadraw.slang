// Shadow map rendering from canonical merged geometry. Unlike shadow.slang,
// this stage has no per-mesh vertex input or LocalUniforms model matrix: the
// merge has already produced world-space positions and one indexed buffer.

import uniforms;
import lib.megadraw_geometry;
import lib.scene_schema;

static const float kPointShadowFar = 2500.0;
// Point shadows replace raster depth with radial distance. Pipeline depth bias
// is therefore replaced along with it, so reproduce the same constant+slope
// model in radial-depth units. Receiver normal offset now handles most grazing
// cases, leaving only a small write-side slope term here.
static const float kPointShadowConstantBias = 0.0;
static const float kPointShadowSlopeBias = 1.0;

struct DirectionalShadowVaryings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float vertexAlpha : TEXCOORD1;
};

[shader("vertex")]
DirectionalShadowVaryings directionalShadowMegadrawVertex(
    uint vertexId : SV_VertexID, uint viewId : SV_ViewID) {
    MergedVertex vertex = mergedVertices[vertexId];
    DirectionalShadowVaryings output;
    output.position = mul(globalUniforms.shadowLightSpace[viewId],
                          float4(vertex.position, 1.0));
    output.uv = vertex.uv1;
    output.vertexAlpha = vertex.color.a;
    return output;
}

/**
 * The two gates a caster has to pass: which objects this mode lets cast at
 * all, and - on the gated range - whether the texel is there.
 *
 * The caster set is a category filter rather than a separate submission
 * because the merge produces one buffer per frame ordered by coverage, not by
 * object kind; splitting it by category would rebuild that ordering on the CPU
 * every frame to save a fragment-shader test that costs nothing when nothing
 * is filtered. Whole-category filtering also means the range stays contiguous,
 * so the draw count does not depend on how many creatures are in the room.
 */
void applyShadowGates(float2 rawUv, float vertexAlpha, uint primitiveId) {
    // Nothing filtered and no coverage gate: the common case reads no memory.
    if (pushConstants.casterCategories == kMegaCastersAll &&
        pushConstants.materialGated == kMegaCoverageUngated) {
        return;
    }
    uint triangleId = pushConstants.triangleBase + primitiveId;
    uint materialIndex = mergedMaterialIds[triangleId];
    InstanceMaterial material = instanceMaterials[materialIndex];

    uint category = (material.featureMask >> kMegaCategoryShift) & kMegaCategoryMask;
    if ((pushConstants.casterCategories & (1u << category)) == 0) discard;

    if (pushConstants.materialGated == kMegaCoverageUngated) return;
    float2 uv = transformedUV(material, rawUv);
    if (!commitsTracedCoverage(material, uv, vertexAlpha)) discard;
}

[shader("fragment")]
void directionalShadowMegadrawFragment(DirectionalShadowVaryings input,
                                       uint primitiveId : SV_PrimitiveID) {
    applyShadowGates(input.uv, input.vertexAlpha, primitiveId);
}

struct PointShadowVaryings {
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float vertexAlpha : TEXCOORD2;
};

[shader("vertex")]
PointShadowVaryings pointShadowMegadrawVertex(
    uint vertexId : SV_VertexID, uint viewId : SV_ViewID) {
    MergedVertex vertex = mergedVertices[vertexId];
    PointShadowVaryings output;
    output.worldPos = vertex.position;
    output.position = mul(globalUniforms.shadowLightSpace[viewId],
                          float4(vertex.position, 1.0));
    output.uv = vertex.uv1;
    output.vertexAlpha = vertex.color.a;
    return output;
}

[shader("fragment")]
float pointShadowMegadrawFragment(PointShadowVaryings input,
                                  uint primitiveId : SV_PrimitiveID) : SV_Depth {
    applyShadowGates(input.uv, input.vertexAlpha, primitiveId);
    float lightDistance = length(input.worldPos -
                                 globalUniforms.shadowLightPosition.xyz);
    float radialDepth = lightDistance / kPointShadowFar;
    float radialSlope = max(abs(ddx(radialDepth)), abs(ddy(radialDepth)));
    return min(1.0, radialDepth + kPointShadowConstantBias +
                        kPointShadowSlopeBias * radialSlope);
}
