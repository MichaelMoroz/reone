// Primary-ray diagnostic for the Vulkan path-tracing render mode.  A ray query
// needs neither a shader binding table nor a ray-tracing pipeline.
import uniforms;

[[vk::binding(0, 1)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] RaytracingAccelerationStructure sceneTLAS;

float3 instanceColor(uint index) {
    // The stable SceneNodeId index makes missing or duplicated TLAS instances
    // immediately visible, unlike a binary hit/miss view.
    uint h = index * 1664525u + 1013904223u;
    return float3((h >> 0) & 255u, (h >> 8) & 255u, (h >> 16) & 255u) / 255.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;

    float2 ndc = ((float2(dispatchId.xy) + 0.5) / float2(width, height)) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 eye = mul(globalUniforms.projectionInv, float4(ndc, 1.0, 1.0));
    eye /= eye.w;
    RayDesc ray;
    ray.Origin = globalUniforms.cameraPosition.xyz;
    ray.Direction = normalize(mul(globalUniforms.viewInv, float4(eye.xyz, 0.0)).xyz);
    ray.TMin = 0.01;
    ray.TMax = 100000.0;

    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) {}
    float3 color = float3(0.02, 0.03, 0.06);
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        color = instanceColor(query.CommittedInstanceID());
    }
    outputImage[dispatchId.xy] = float4(color, 1.0);
}
