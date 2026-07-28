// Primary-ray diagnostic for the Vulkan path-tracing render mode.  A ray query
// needs neither a shader binding table nor a ray-tracing pipeline.
import uniforms;

[[vk::binding(0, 1)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] RaytracingAccelerationStructure sceneTLAS;

struct InstanceMaterial {
    // Kept as a colour rather than a boolean because later path-tracing work
    // adds albedo and emission intensity alongside it.
    float4 selfIllumColor;
};
[[vk::binding(2, 1)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 1)]] RWStructuredBuffer<uint> traceStats;

struct PushConstants {
    uint frameIndex;
    // Runtime rather than a compile-time constant so the cost/noise trade can
    // be moved without a shader rebuild - and a shader rebuild here needs the
    // SPIR-V transpile, which a named build target skips.
    uint samplesPerPixel;
};
[[vk::push_constant]] PushConstants pushConstants;


static const float kRayOriginOffset = 0.01;

uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float random01(inout uint state) {
    state = hash(state);
    return float(state & 0x00ffffffu) / 16777216.0;
}

bool isEmissive(uint instanceIndex) {
    float3 rgb = instanceMaterials[instanceIndex].selfIllumColor.xyz;
    return dot(rgb, float3(0.299, 0.587, 0.114)) > 0.99;
}

float3 cosineHemisphere(float3 normal, float2 xi) {
    float radius = sqrt(xi.x);
    float phi = 6.28318530718 * xi.y;
    float3 local = float3(radius * cos(phi), radius * sin(phi), sqrt(1.0 - xi.x));
    float3 reference = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 tangent = normalize(cross(reference, normal));
    float3 bitangent = cross(normal, tangent);
    return tangent * local.x + bitangent * local.y + normal * local.z;
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

    RayQuery<RAY_FLAG_NONE> primary;
    primary.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    while (primary.Proceed()) {}

    float3 color = 0.0;
    if (primary.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        // InstanceIndex is the dense TLAS-array position. InstanceID is the
        // 24-bit custom SceneNodeId index, so using it here would index this
        // buffer out of bounds for almost every scene hit.
        uint primaryInstance = primary.CommittedInstanceIndex();
        if (isEmissive(primaryInstance)) {
            // A camera ray directly landing on the sky is already at the
            // emitter: it does not need a second bounce to read as white.
            color = 1.0;
        } else {
            // Slang emits OpRayQueryGetIntersectionTriangleVertexPositionsKHR
            // here, enabled by VK_KHR_ray_tracing_position_fetch.
            float3 positions[3] = primary.CommittedGetIntersectionTriangleVertexPositions();
            float3x4 objectToWorld = primary.CommittedObjectToWorld3x4();
            float3 p0 = mul(objectToWorld, float4(positions[0], 1.0));
            float3 p1 = mul(objectToWorld, float4(positions[1], 1.0));
            float3 p2 = mul(objectToWorld, float4(positions[2], 1.0));
            float3 normal = normalize(cross(p1 - p0, p2 - p0));
            if (dot(normal, ray.Direction) > 0.0) normal = -normal;
            float3 hitPosition = ray.Origin + ray.Direction * primary.CommittedRayT();

            float illumination = 0.0;
            uint sampleCount = max(pushConstants.samplesPerPixel, 1u);
            for (uint sample = 0; sample < sampleCount; ++sample) {
                uint state = hash(dispatchId.x * 0x9e3779b9u ^ dispatchId.y * 0x85ebca6bu ^
                                  sample * 0xc2b2ae35u ^ pushConstants.frameIndex);
                RayDesc secondary;
                secondary.Origin = hitPosition + normal * kRayOriginOffset;
                secondary.Direction = cosineHemisphere(normal, float2(random01(state), random01(state)));
                secondary.TMin = 0.001;
                secondary.TMax = 100000.0;
                RayQuery<RAY_FLAG_NONE> bounce;
                bounce.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, secondary);
                while (bounce.Proceed()) {}
                InterlockedAdd(traceStats[0], 1);
                if (bounce.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                    illumination += isEmissive(bounce.CommittedInstanceIndex()) ? 1.0 : 0.0;
                } else {
                    InterlockedAdd(traceStats[1], 1);
                }
            }
            color = illumination / float(sampleCount);
        }
    }
    outputImage[dispatchId.xy] = float4(color, 1.0);
}
