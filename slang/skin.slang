// One workgroup stream builds the whole frame's canonical ray-query mesh.
//
// Geometry is copied into one vertex/index stream because the acceleration
// structure has one BLAS.  This keeps the object-to-source mapping here,
// rather than duplicating per-mesh tracing shaders.
import lib.geometry;

struct Matrix3x4 {
    float4 row0;
    float4 row1;
    float4 row2;
};

struct SceneObject {
    Matrix3x4 transform;
    Matrix3x4 prevTransform;
    uint64_t srcVertexAddress;
    uint64_t srcIndexAddress;
    uint srcVertexStride;
    int offPosition;
    int offNormals;
    int offUV1;
    int offUV2;
    int offTanSpace;
    int offBoneIndices;
    int offBoneWeights;
    uint vertexCount;
    uint triangleCount;
    uint dstVertexBase;
    uint dstTriangleBase;
    uint geometryIndex;
    uint boneBase;
    uint boneCount;
    uint materialIndex;
};

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
};

[[vk::binding(0, 0)]] StructuredBuffer<SceneObject> objects;
[[vk::binding(1, 0)]] StructuredBuffer<Matrix3x4> bones;
[[vk::binding(2, 0)]] RWStructuredBuffer<MergedVertex> vertices;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> indices;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> materialIds;

struct PushConstants {
    uint objectCount;
    uint opaqueObjectCount;
    uint vertexCount;
    uint triangleCount;
    uint opaqueTriangleCount;
};

[[vk::push_constant]] PushConstants pushConstants;

float loadFloat(uint64_t address) {
    ConstBufferPointer<uint, 4> pointer = ConstBufferPointer<uint, 4>(address);
    return asfloat(pointer[0]);
}

float3 load3(uint64_t address) {
    return float3(
        loadFloat(address),
        loadFloat(address + 4),
        loadFloat(address + 8));
}

float2 load2(uint64_t address) {
    return float2(loadFloat(address), loadFloat(address + 4));
}

uint loadIndex16(uint64_t address, uint index) {
    uint64_t byteAddress = address + uint64_t(index) * 2;

    // The source index stream is 16-bit, but raw loads are dwords.  Aligning
    // down and selecting the low or high half handles either unaligned index
    // address without assuming that the source buffer itself is dword-aligned.
    ConstBufferPointer<uint, 4> pointer = ConstBufferPointer<uint, 4>(byteAddress & ~uint64_t(3));
    return (pointer[0] >> (uint(byteAddress & 2) * 8)) & 0xffffu;
}

float3 transformPoint(Matrix3x4 matrix, float3 point) {
    return float3(
        dot(matrix.row0, float4(point, 1)),
        dot(matrix.row1, float4(point, 1)),
        dot(matrix.row2, float4(point, 1)));
}

float3 transformDir(Matrix3x4 matrix, float3 direction) {
    return float3(
        dot(matrix.row0.xyz, direction),
        dot(matrix.row1.xyz, direction),
        dot(matrix.row2.xyz, direction));
}

float3 safeNormalize(float3 value, float3 fallback) {
    float lengthSquared = dot(value, value);
    return lengthSquared > 0.0 ? value * rsqrt(lengthSquared) : fallback;
}

// The C++ upload packs one object's current palette immediately followed by
// its previous palette: bones[boneBase + b] is current and
// bones[boneBase + boneCount + b] is previous.  Keeping that addressing,
// weight application, and index clamping in one helper makes position and
// shading-frame skinning impossible to drift apart.
float3 skinVertex(
    SceneObject object,
    float4 boneIndices,
    float4 boneWeights,
    float4 value,
    bool previous) {
    uint paletteBase = object.boneBase + (previous ? object.boneCount : 0);
    float3 result = float3(0);

    [unroll]
    for (uint influence = 0; influence < 4; ++influence) {
        uint bone = min(
            uint(max(0, int(boneIndices[influence]))),
            object.boneCount - 1);
        Matrix3x4 matrix = bones[paletteBase + bone];
        float3 transformed = float3(
            dot(matrix.row0, value),
            dot(matrix.row1, value),
            dot(matrix.row2, value));
        result += transformed * boneWeights[influence];
    }

    return result;
}

// Objects are concatenated geometry-0-first.  Consequently dstVertexBase is
// globally monotonic across the object array, so the greatest base no larger
// than the output vertex id is its owner.
uint findVertexObject(uint id) {
    uint low = 0;
    uint high = pushConstants.objectCount;

    while (low + 1 < high) {
        uint middle = (low + high) / 2;

        if (objects[middle].dstVertexBase <= id) {
            low = middle;
        } else {
            high = middle;
        }
    }

    return low;
}

// Objects are likewise geometry-0-first, and dstTriangleBase is monotonic
// within each geometry.  Vertices use a global dstVertexBase, while
// dstTriangleBase restarts at zero for each geometry; search only the owning
// geometry after converting the global triangle id to its local id.
uint findTriangleObject(uint id) {
    uint geometry = id >= pushConstants.opaqueTriangleCount ? 1 : 0;
    uint localId = id - (geometry == 0 ? 0 : pushConstants.opaqueTriangleCount);
    uint low = geometry == 0 ? 0 : pushConstants.opaqueObjectCount;
    uint high = geometry == 0 ? pushConstants.opaqueObjectCount : pushConstants.objectCount;

    while (low + 1 < high) {
        uint middle = (low + high) / 2;

        if (objects[middle].dstTriangleBase <= localId) {
            low = middle;
        } else {
            high = middle;
        }
    }

    return low;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint id = tid.x;

    if (id < pushConstants.vertexCount) {
        SceneObject object = objects[findVertexObject(id)];
        uint vertexId = id - object.dstVertexBase;
        uint64_t address = object.srcVertexAddress + uint64_t(vertexId) * object.srcVertexStride;
        float3 position = load3(address + uint64_t(object.offPosition));
        float3 prevPosition = position;
        float4 boneIndices = float4(0);
        float4 boneWeights = float4(0);
        bool skinned = object.boneBase != 0xffffffffu
            && object.offBoneIndices >= 0
            && object.offBoneWeights >= 0
            && object.boneCount > 0;

        if (skinned) {
            boneIndices = float4(
                loadFloat(address + uint64_t(object.offBoneIndices)),
                loadFloat(address + uint64_t(object.offBoneIndices + 4)),
                loadFloat(address + uint64_t(object.offBoneIndices + 8)),
                loadFloat(address + uint64_t(object.offBoneIndices + 12)));
            boneWeights = float4(
                loadFloat(address + uint64_t(object.offBoneWeights)),
                loadFloat(address + uint64_t(object.offBoneWeights + 4)),
                loadFloat(address + uint64_t(object.offBoneWeights + 8)),
                loadFloat(address + uint64_t(object.offBoneWeights + 12)));

            // Positions have w = 1, so the bone's translation deforms them.
            // Directions have w = 0 below, so normals and tangent-frame axes
            // inherit only rotation/scale and never a bone translation.
            position = skinVertex(
                object,
                boneIndices,
                boneWeights,
                float4(position, 1),
                false);
            prevPosition = skinVertex(
                object,
                boneIndices,
                boneWeights,
                float4(prevPosition, 1),
                true);
        }

        MergedVertex mergedVertex;
        mergedVertex.position = transformPoint(object.transform, position);
        mergedVertex.prevPosition = transformPoint(object.prevTransform, prevPosition);

        // Some legacy meshes omit normals.  A stable axis is less faithful
        // than a face normal, but it is a documented, finite fallback and
        // avoids sending a zero vector into material or BRDF evaluation.
        float3 normal = object.offNormals < 0
            ? float3(0, 0, 1)
            : load3(address + uint64_t(object.offNormals));

        if (skinned) {
            normal = skinVertex(
                object,
                boneIndices,
                boneWeights,
                float4(normal, 0),
                false);
        }

        mergedVertex.normal = safeNormalize(
            transformDir(object.transform, normal),
            float3(0, 0, 1));
        mergedVertex.uv1 = object.offUV1 < 0
            ? float2(0)
            : load2(address + uint64_t(object.offUV1));
        mergedVertex.uv2 = object.offUV2 < 0
            ? float2(0)
            : load2(address + uint64_t(object.offUV2));
        mergedVertex.bitangent = float3(0);
        mergedVertex.tangent = float3(0);
        mergedVertex.tanSpaceNormal = float3(0);

        if (object.offTanSpace >= 0) {
            float3 bitangent = load3(address + uint64_t(object.offTanSpace));
            float3 tangent = load3(address + uint64_t(object.offTanSpace + 12));
            float3 tanSpaceNormal = load3(address + uint64_t(object.offTanSpace + 24));

            if (skinned) {
                bitangent = skinVertex(
                    object,
                    boneIndices,
                    boneWeights,
                    float4(bitangent, 0),
                    false);
                tangent = skinVertex(
                    object,
                    boneIndices,
                    boneWeights,
                    float4(tangent, 0),
                    false);
                tanSpaceNormal = skinVertex(
                    object,
                    boneIndices,
                    boneWeights,
                    float4(tanSpaceNormal, 0),
                    false);
            }

            mergedVertex.bitangent = safeNormalize(
                transformDir(object.transform, bitangent),
                float3(0, 1, 0));
            mergedVertex.tangent = safeNormalize(
                transformDir(object.transform, tangent),
                float3(1, 0, 0));
            mergedVertex.tanSpaceNormal = safeNormalize(
                transformDir(object.transform, tanSpaceNormal),
                float3(0, 0, 1));
        }

        mergedVertex.pad = float2(0);
        vertices[id] = mergedVertex;
    }

    if (id < pushConstants.triangleCount) {
        SceneObject object = objects[findTriangleObject(id)];
        uint localId = id
            - (object.geometryIndex == 0 ? 0 : pushConstants.opaqueTriangleCount)
            - object.dstTriangleBase;
        uint firstIndex = localId * 3;
        uint destinationIndex = id * 3;

        indices[destinationIndex] = loadIndex16(object.srcIndexAddress, firstIndex)
            + object.dstVertexBase;
        indices[destinationIndex + 1] = loadIndex16(object.srcIndexAddress, firstIndex + 1)
            + object.dstVertexBase;
        indices[destinationIndex + 2] = loadIndex16(object.srcIndexAddress, firstIndex + 2)
            + object.dstVertexBase;
        materialIds[id] = object.materialIndex;
    }
}
