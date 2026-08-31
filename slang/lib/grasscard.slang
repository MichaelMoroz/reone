// One grass card is a template vertex under an affine instance transform.
// Keep this expansion here: raster and traced cards must not acquire subtly
// different positions, UVs, or normals by carrying local copies of the math.

import lib.scene_schema;
import lib.math;

public struct GrassCardVertex {
    public float3 position;
    public float3 prevPosition;
    public float3 normal;
    public float2 uv;
    public float2 lightmapUV;
    public bool culled;
};

public GrassCardVertex resolveGrassCardVertex(GrassCardInstance card,
                                              float4 templateVertex,
                                              float3 cameraPosition) {
    float4 local = float4(templateVertex.xy, 0.0, 1.0);
    GrassCardVertex result;
    result.position = float3(dot(card.row0, local), dot(card.row1, local),
                             dot(card.row2, local));
    result.prevPosition = float3(dot(card.prevRow0, local), dot(card.prevRow1, local),
                                 dot(card.prevRow2, local));
    float3 normal = cross(float3(card.row0.y, card.row1.y, card.row2.y),
                          float3(card.row0.x, card.row1.x, card.row2.x));
    float normalLengthSquared = dot(normal, normal);
    normal = normalLengthSquared > 1.0e-10
        ? normal * rsqrt(normalLengthSquared)
        : float3(0.0, 0.0, 1.0);
    result.culled = card.lightmapVariantCulled.w != 0.0;
    result.normal = result.culled || dot(normal, cameraPosition - result.position) < 0.0
        ? -normal : normal;
    uint variant = uint(card.lightmapVariantCulled.z);
    result.uv = float2(0.5 * float(variant % 2), 0.5 * float(variant / 2)) +
                templateVertex.zw * 0.5;
    result.lightmapUV = card.lightmapVariantCulled.xy;
    return result;
}
