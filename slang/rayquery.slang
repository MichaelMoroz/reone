// Primary-ray path tracer for Vulkan ray query.  Every hit reads its material
// and vertex attributes from GPU-resident data; no raster draw state is used.
import uniforms;

[[vk::binding(0, 1)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] RaytracingAccelerationStructure sceneTLAS;

static const uint kNoTexture = 0xffffffffu;

struct InstanceMaterial {
    float4 selfIllumColor;
    float4 diffuseColor;
    float4 uv0;
    float4 uv1;
    float4 uv2;
    uint64_t vertexAddress;
    uint64_t indexAddress;
    uint vertexStride;
    int offPosition;
    int offNormals;
    int offUV1;
    int offTanSpace;
    uint mainTex;
    uint normalMap;
    uint featureMask;
};
[[vk::binding(2, 1)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 1)]] RWStructuredBuffer<uint> traceStats;
// This binding is deliberately both runtime-sized and non-uniformly indexed.
// The descriptor layout supplies UPDATE_AFTER_BIND and PARTIALLY_BOUND.
[[vk::binding(4, 1)]] Sampler2D bindlessTextures[];

struct PushConstants {
    uint frameIndex;
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

float loadFloat(uint64_t address) {
    // This Slang version exposes physical storage through ConstBufferPointer
    // rather than DXC's vk::RawBufferLoad. The explicit 4-byte alignment is
    // the same raw-address operation, one component at a time.
    ConstBufferPointer<uint, 4> pointer = ConstBufferPointer<uint, 4>(address);
    return asfloat(pointer[0]);
}

float2 loadFloat2(uint64_t address) {
    return float2(loadFloat(address), loadFloat(address + 4));
}

float3 loadFloat3(uint64_t address) {
    return float3(loadFloat(address), loadFloat(address + 4), loadFloat(address + 8));
}

uint loadIndex16(uint64_t indexAddress, uint index) {
    uint64_t address = indexAddress + uint64_t(index) * 2;
    ConstBufferPointer<uint, 4> pointer = ConstBufferPointer<uint, 4>(address & ~uint64_t(3));
    uint packed = pointer[0];
    return (packed >> (uint(address & 2) * 8)) & 0xffffu;
}

uint3 triangleIndices(InstanceMaterial material, uint primitive) {
    uint first = primitive * 3;
    return uint3(loadIndex16(material.indexAddress, first),
                 loadIndex16(material.indexAddress, first + 1),
                 loadIndex16(material.indexAddress, first + 2));
}

uint64_t attributeAddress(InstanceMaterial material, uint vertex, int offset) {
    return material.vertexAddress + uint64_t(vertex) * uint64_t(material.vertexStride) + uint64_t(offset);
}

float2 interpolateFloat2(InstanceMaterial material, uint3 indices, int offset, float3 weights) {
    return loadFloat2(attributeAddress(material, indices.x, offset)) * weights.x +
           loadFloat2(attributeAddress(material, indices.y, offset)) * weights.y +
           loadFloat2(attributeAddress(material, indices.z, offset)) * weights.z;
}

float3 interpolateFloat3(InstanceMaterial material, uint3 indices, int offset, float3 weights) {
    return loadFloat3(attributeAddress(material, indices.x, offset)) * weights.x +
           loadFloat3(attributeAddress(material, indices.y, offset)) * weights.y +
           loadFloat3(attributeAddress(material, indices.z, offset)) * weights.z;
}

float2 transformedUV(InstanceMaterial material, float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(material.uv0.x * h.x + material.uv1.x * h.y + material.uv2.x * h.z,
                  material.uv0.y * h.x + material.uv1.y * h.y + material.uv2.y * h.z);
}

float3 emission(uint instanceIndex) {
    return instanceMaterials[instanceIndex].selfIllumColor.rgb;
}

bool isEmitter(float3 e) {
    return any(e > 0.0);
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

float3 sampleShadingNormal(InstanceMaterial material, uint3 indices, float3 weights,
                           float3 geometricNormal, float3 rayDirection, float2 uv) {
    float3 shadingNormal = geometricNormal;
    if (material.offNormals != -1) {
        shadingNormal = normalize(interpolateFloat3(material, indices, material.offNormals, weights));
    }
    if (dot(shadingNormal, rayDirection) > 0.0) shadingNormal = -shadingNormal;

    if (material.offTanSpace == -1 || material.normalMap == kNoTexture ||
        (material.featureMask & uint(kFeatureNormalMap)) == 0) {
        return shadingNormal;
    }

    // offTanSpace is bitangent, tangent, tangent-space normal in that order.
    float3 interpolatedBitangent = interpolateFloat3(material, indices, material.offTanSpace, weights);
    float3 tangent = interpolateFloat3(material, indices, material.offTanSpace + 12, weights);
    float3 tangentSpaceNormal = interpolateFloat3(material, indices, material.offTanSpace + 24, weights);
    tangent = normalize(tangent - shadingNormal * dot(tangent, shadingNormal));
    float handedness = dot(cross(shadingNormal, tangent), interpolatedBitangent) < 0.0 ? -1.0 : 1.0;
    float3 bitangent = cross(shadingNormal, tangent) * handedness;
    // The third vector is normally the same smooth normal as offNormals. It
    // still guards corrupt/degenerate tangent frames before sampling them.
    if (dot(tangentSpaceNormal, tangentSpaceNormal) < 1e-10 ||
        dot(tangent, tangent) < 1e-10 || dot(bitangent, bitangent) < 1e-10) {
        return shadingNormal;
    }
    // Compute shaders have no screen-space derivatives, so this must select a
    // level explicitly rather than requesting the compute-derivatives extension.
    float3 normalTexel = bindlessTextures[NonUniformResourceIndex(material.normalMap)].SampleLevel(uv, 0.0).rgb * 2.0 - 1.0;
    return normalize(tangent * normalTexel.x + bitangent * normalTexel.y + shadingNormal * normalTexel.z);
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
        uint primaryInstance = primary.CommittedInstanceIndex();
        InstanceMaterial material = instanceMaterials[primaryInstance];
        float3 primaryEmission = material.selfIllumColor.rgb;
        if (isEmitter(primaryEmission)) {
            color = primaryEmission;
        } else {
            uint3 indices = triangleIndices(material, primary.CommittedPrimitiveIndex());
            float2 barycentrics = primary.CommittedTriangleBarycentrics();
            float3 weights = float3(1.0 - barycentrics.x - barycentrics.y,
                                    barycentrics.x, barycentrics.y);
            float3x4 objectToWorld = primary.CommittedObjectToWorld3x4();
            float3x4 worldToObject = primary.CommittedWorldToObject3x4();
            float3 p0 = mul(objectToWorld, float4(loadFloat3(attributeAddress(material, indices.x, material.offPosition)), 1.0));
            float3 p1 = mul(objectToWorld, float4(loadFloat3(attributeAddress(material, indices.y, material.offPosition)), 1.0));
            float3 p2 = mul(objectToWorld, float4(loadFloat3(attributeAddress(material, indices.z, material.offPosition)), 1.0));
            float3 geometricNormal = normalize(cross(p1 - p0, p2 - p0));
            if (dot(geometricNormal, ray.Direction) > 0.0) geometricNormal = -geometricNormal;
            float2 uv = float2(0.0);
            if (material.offUV1 != -1) {
                uv = transformedUV(material, interpolateFloat2(material, indices, material.offUV1, weights));
            }
            float3 shadingNormal = sampleShadingNormal(material, indices, weights, geometricNormal,
                                                       ray.Direction, uv);
            // Attributes are object-space; transform the interpolated normal
            // by the inverse-transpose represented as a row-vector multiply.
            shadingNormal = normalize(mul(shadingNormal, (float3x3)worldToObject));
            if (dot(shadingNormal, ray.Direction) > 0.0) shadingNormal = -shadingNormal;
            float3 albedo = material.diffuseColor.rgb;
            if (material.offUV1 != -1 && material.mainTex != kNoTexture) {
                albedo *= bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleLevel(uv, 0.0).rgb;
            }
            float3 hitPosition = ray.Origin + ray.Direction * primary.CommittedRayT();

            float3 illumination = 0.0;
            uint sampleCount = max(pushConstants.samplesPerPixel, 1u);
            for (uint sample = 0; sample < sampleCount; ++sample) {
                uint state = hash(dispatchId.x * 0x9e3779b9u ^ dispatchId.y * 0x85ebca6bu ^
                                  sample * 0xc2b2ae35u ^ pushConstants.frameIndex);
                RayDesc secondary;
                secondary.Origin = hitPosition + geometricNormal * kRayOriginOffset;
                secondary.Direction = cosineHemisphere(shadingNormal, float2(random01(state), random01(state)));
                // A smooth normal can sample below the geometric surface at a
                // silhouette. Reflect that lobe into the geometric hemisphere
                // rather than immediately self-intersecting the same triangle.
                if (dot(secondary.Direction, geometricNormal) < 0.0) {
                    secondary.Direction = normalize(secondary.Direction -
                        2.0 * dot(secondary.Direction, geometricNormal) * geometricNormal);
                }
                secondary.TMin = 0.001;
                secondary.TMax = 100000.0;
                RayQuery<RAY_FLAG_NONE> bounce;
                bounce.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, secondary);
                while (bounce.Proceed()) {}
                InterlockedAdd(traceStats[0], 1);
                if (bounce.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                    illumination += emission(bounce.CommittedInstanceIndex());
                } else {
                    InterlockedAdd(traceStats[1], 1);
                }
            }
            color = albedo * illumination / float(sampleCount);
        }
    }
    outputImage[dispatchId.xy] = float4(color, 1.0);
}
