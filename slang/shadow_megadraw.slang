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

void applyDirectionalShadowCoverage(DirectionalShadowVaryings input,
                                    uint primitiveId) {
    if (pushConstants.materialGated == 0) return;

    uint triangleId = pushConstants.triangleBase + primitiveId;
    uint materialIndex = mergedMaterialIds[triangleId];
    InstanceMaterial material = instanceMaterials[materialIndex];
    float2 uv = transformedUV(material, input.uv);
    if (!commitsTracedCoverage(material, uv, input.vertexAlpha)) discard;
}

[shader("fragment")]
void directionalShadowMegadrawFragment(DirectionalShadowVaryings input,
                                       uint primitiveId : SV_PrimitiveID) {
    applyDirectionalShadowCoverage(input, primitiveId);
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

void applyPointShadowCoverage(PointShadowVaryings input, uint primitiveId) {
    if (pushConstants.materialGated == 0) return;

    uint triangleId = pushConstants.triangleBase + primitiveId;
    uint materialIndex = mergedMaterialIds[triangleId];
    InstanceMaterial material = instanceMaterials[materialIndex];
    float2 uv = transformedUV(material, input.uv);
    if (!commitsTracedCoverage(material, uv, input.vertexAlpha)) discard;
}

[shader("fragment")]
float pointShadowMegadrawFragment(PointShadowVaryings input,
                                  uint primitiveId : SV_PrimitiveID) : SV_Depth {
    applyPointShadowCoverage(input, primitiveId);
    float lightDistance = length(input.worldPos -
                                 globalUniforms.shadowLightPosition.xyz);
    float radialDepth = lightDistance / kPointShadowFar;
    float radialSlope = max(abs(ddx(radialDepth)), abs(ddy(radialDepth)));
    return min(1.0, radialDepth + kPointShadowConstantBias +
                        kPointShadowSlopeBias * radialSlope);
}
