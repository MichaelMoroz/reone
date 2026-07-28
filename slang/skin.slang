// Frame-local skinning for the ray-query path.  It deliberately calls the
// same skinPosition helper as pbr_model.slang so the snapshot bone palette,
// weight layout, index conversion, and matrix convention cannot drift.
import uniforms;
import lib.geometry;

[[vk::binding(0, 1)]] StructuredBuffer<float> sourceVertices;
[[vk::binding(1, 1)]] RWStructuredBuffer<float> skinnedVertices;

struct PushConstants {
    uint vertexCount;
    uint vertexStrideFloats;
    int positionOffsetFloats;
    int normalOffsetFloats;
    int boneIndicesOffsetFloats;
    int boneWeightsOffsetFloats;
    // The tracing shader derives its shading normal from the tangent-space
    // block whenever one exists, so leaving it rest-posed lit the deformed
    // body with rest-pose normals - patchwork shading on every character.
    int tanSpaceOffsetFloats;
};
[[vk::push_constant]] PushConstants pushConstants;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint vertex = dispatchId.x;
    if (vertex >= pushConstants.vertexCount) {
        return;
    }

    uint base = vertex * pushConstants.vertexStrideFloats;
    // Preserve the source record byte-for-byte apart from the deformed
    // attributes. This retains the source stride and every material/UV/tangent
    // offset used directly by InstanceMaterial in the tracing shader.
    for (uint i = 0; i < pushConstants.vertexStrideFloats; ++i) {
        skinnedVertices[base + i] = sourceVertices[base + i];
    }

    if (pushConstants.positionOffsetFloats < 0 ||
        pushConstants.boneIndicesOffsetFloats < 0 ||
        pushConstants.boneWeightsOffsetFloats < 0) {
        return;
    }

    uint positionOffset = uint(pushConstants.positionOffsetFloats);
    uint indicesOffset = uint(pushConstants.boneIndicesOffsetFloats);
    uint weightsOffset = uint(pushConstants.boneWeightsOffsetFloats);
    VertexSkin skin;
    skin.indices = float4(sourceVertices[base + indicesOffset],
                          sourceVertices[base + indicesOffset + 1],
                          sourceVertices[base + indicesOffset + 2],
                          sourceVertices[base + indicesOffset + 3]);
    skin.weights = float4(sourceVertices[base + weightsOffset],
                          sourceVertices[base + weightsOffset + 1],
                          sourceVertices[base + weightsOffset + 2],
                          sourceVertices[base + weightsOffset + 3]);

    float4 position = skinPosition(float4(sourceVertices[base + positionOffset],
                                          sourceVertices[base + positionOffset + 1],
                                          sourceVertices[base + positionOffset + 2], 1.0), skin, false);
    skinnedVertices[base + positionOffset] = position.x;
    skinnedVertices[base + positionOffset + 1] = position.y;
    skinnedVertices[base + positionOffset + 2] = position.z;

    if (pushConstants.normalOffsetFloats >= 0) {
        uint normalOffset = uint(pushConstants.normalOffsetFloats);
        float4 normal = skinPosition(float4(sourceVertices[base + normalOffset],
                                            sourceVertices[base + normalOffset + 1],
                                            sourceVertices[base + normalOffset + 2], 0.0), skin, false);
        skinnedVertices[base + normalOffset] = normal.x;
        skinnedVertices[base + normalOffset + 1] = normal.y;
        skinnedVertices[base + normalOffset + 2] = normal.z;
    }

    if (pushConstants.tanSpaceOffsetFloats >= 0) {
        // Bitangent, tangent, tangent-space normal - three direction vectors,
        // three floats apart, all bone-transformed with w = 0.
        for (uint vec = 0; vec < 3; ++vec) {
            uint offset = uint(pushConstants.tanSpaceOffsetFloats) + vec * 3;
            float4 direction = skinPosition(float4(sourceVertices[base + offset],
                                                   sourceVertices[base + offset + 1],
                                                   sourceVertices[base + offset + 2], 0.0), skin, false);
            skinnedVertices[base + offset] = direction.x;
            skinnedVertices[base + offset + 1] = direction.y;
            skinnedVertices[base + offset + 2] = direction.z;
        }
    }
}
