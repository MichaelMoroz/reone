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
    // Element offsets: floats in sourceVertices and uints in sourceIndices.
    // Attribute offsets below remain byte offsets inside each vertex record.
    uint srcVertexOffset;
    uint srcIndexOffset;
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
    uint danglyBase;
    uint danglyCount;
    float4 saberDisplacement;
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
[[vk::binding(5, 0)]] StructuredBuffer<float> sourceVertices;
[[vk::binding(6, 0)]] StructuredBuffer<uint> sourceIndices;

struct GrassCluster {
    float4 positionVariant;
    float4 right;
    float4 up;
    float2 lightmapUV;
    float2 pad;
};

[[vk::binding(7, 0)]] StructuredBuffer<GrassCluster> grassClusters;
[[vk::binding(8, 0)]] StructuredBuffer<float4> danglyPositions;

struct PushConstants {
    uint objectCount;
    uint opaqueObjectCount;
    uint vertexCount;
    uint triangleCount;
    uint opaqueTriangleCount;
};

[[vk::push_constant]] PushConstants pushConstants;

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
        if (object.srcVertexStride == 0) {
            GrassCluster cluster = grassClusters[object.srcVertexOffset + vertexId / 4];
            uint quadVertex = vertexId % 4;
            float x = (quadVertex == 0 || quadVertex == 3) ? -0.5 : 0.5;
            float y = quadVertex < 2 ? 0.0 : 1.0;
            MergedVertex mergedVertex;
            mergedVertex.position = cluster.positionVariant.xyz + cluster.right.xyz * x + cluster.up.xyz * y;
            mergedVertex.prevPosition = mergedVertex.position;
            mergedVertex.normal = safeNormalize(cross(cluster.right.xyz, cluster.up.xyz), float3(0, 0, 1));
            float2 uv = float2(x + 0.5, y);
            int variant = int(cluster.positionVariant.w);
            mergedVertex.uv1 = 0.5 * uv + float2(0.5 * (variant % 2), 0.5 * (variant / 2));
            mergedVertex.uv2 = cluster.lightmapUV;
            mergedVertex.bitangent = float3(0);
            mergedVertex.tangent = float3(0);
            mergedVertex.tanSpaceNormal = float3(0);
            mergedVertex.pad = float2(0);
            vertices[id] = mergedVertex;
        } else {
        uint sourceVertex = object.srcVertexOffset + vertexId * (object.srcVertexStride / 4);
        float3 position = float3(
            sourceVertices[sourceVertex + object.offPosition / 4],
            sourceVertices[sourceVertex + object.offPosition / 4 + 1],
            sourceVertices[sourceVertex + object.offPosition / 4 + 2]);
        float3 prevPosition = position;
        bool dangly = object.danglyBase != 0xffffffffu
            && object.danglyCount == object.vertexCount;
        if (dangly) {
            position = danglyPositions[object.danglyBase + vertexId].xyz;
            prevPosition = danglyPositions[object.danglyBase + object.danglyCount + vertexId].xyz;
        }
        float saberSignum = 2.0 * float(vertexId / 88) - 1.0;
        float saberHdist = float((vertexId % 88) / 4) / 21.0;
        float saberVdist = float(vertexId % 4) / 3.0;
        float3 saberOffset = 0.5 * saberSignum * saberHdist * saberVdist * object.saberDisplacement.xyz;
        position += saberOffset;
        prevPosition += saberOffset;
        float4 boneIndices = float4(0);
        float4 boneWeights = float4(0);
        bool skinned = object.boneBase != 0xffffffffu
            && object.offBoneIndices >= 0
            && object.offBoneWeights >= 0
            && object.boneCount > 0;

        if (skinned) {
            boneIndices = float4(
                sourceVertices[sourceVertex + object.offBoneIndices / 4],
                sourceVertices[sourceVertex + object.offBoneIndices / 4 + 1],
                sourceVertices[sourceVertex + object.offBoneIndices / 4 + 2],
                sourceVertices[sourceVertex + object.offBoneIndices / 4 + 3]);
            boneWeights = float4(
                sourceVertices[sourceVertex + object.offBoneWeights / 4],
                sourceVertices[sourceVertex + object.offBoneWeights / 4 + 1],
                sourceVertices[sourceVertex + object.offBoneWeights / 4 + 2],
                sourceVertices[sourceVertex + object.offBoneWeights / 4 + 3]);

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
            : float3(
                sourceVertices[sourceVertex + object.offNormals / 4],
                sourceVertices[sourceVertex + object.offNormals / 4 + 1],
                sourceVertices[sourceVertex + object.offNormals / 4 + 2]);

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
            : float2(
                sourceVertices[sourceVertex + object.offUV1 / 4],
                sourceVertices[sourceVertex + object.offUV1 / 4 + 1]);
        mergedVertex.uv2 = object.offUV2 < 0
            ? float2(0)
            : float2(
                sourceVertices[sourceVertex + object.offUV2 / 4],
                sourceVertices[sourceVertex + object.offUV2 / 4 + 1]);
        mergedVertex.bitangent = float3(0);
        mergedVertex.tangent = float3(0);
        mergedVertex.tanSpaceNormal = float3(0);

        if (object.offTanSpace >= 0) {
            float3 bitangent = float3(
                sourceVertices[sourceVertex + object.offTanSpace / 4],
                sourceVertices[sourceVertex + object.offTanSpace / 4 + 1],
                sourceVertices[sourceVertex + object.offTanSpace / 4 + 2]);
            float3 tangent = float3(
                sourceVertices[sourceVertex + object.offTanSpace / 4 + 3],
                sourceVertices[sourceVertex + object.offTanSpace / 4 + 4],
                sourceVertices[sourceVertex + object.offTanSpace / 4 + 5]);
            float3 tanSpaceNormal = float3(
                sourceVertices[sourceVertex + object.offTanSpace / 4 + 6],
                sourceVertices[sourceVertex + object.offTanSpace / 4 + 7],
                sourceVertices[sourceVertex + object.offTanSpace / 4 + 8]);

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
    }

    if (id < pushConstants.triangleCount) {
        SceneObject object = objects[findTriangleObject(id)];
        uint localId = id
            - (object.geometryIndex == 0 ? 0 : pushConstants.opaqueTriangleCount)
            - object.dstTriangleBase;
        uint destinationIndex = id * 3;
        if (object.srcVertexStride == 0) {
            uint clusterBase = object.dstVertexBase + (localId / 2) * 4;
            if ((localId % 2) == 0) {
                indices[destinationIndex] = clusterBase;
                indices[destinationIndex + 1] = clusterBase + 1;
                indices[destinationIndex + 2] = clusterBase + 2;
            } else {
                indices[destinationIndex] = clusterBase + 2;
                indices[destinationIndex + 1] = clusterBase + 3;
                indices[destinationIndex + 2] = clusterBase;
            }
        } else {
            uint firstIndex = localId * 3;
            indices[destinationIndex] = sourceIndices[object.srcIndexOffset + firstIndex]
                + object.dstVertexBase;
            indices[destinationIndex + 1] = sourceIndices[object.srcIndexOffset + firstIndex + 1]
                + object.dstVertexBase;
            indices[destinationIndex + 2] = sourceIndices[object.srcIndexOffset + firstIndex + 2]
                + object.dstVertexBase;
        }
        materialIds[id] = object.materialIndex;
    }
}
