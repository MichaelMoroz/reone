/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
// One workgroup stream builds the whole frame's canonical ray-query mesh.
//
// Geometry is copied into one vertex/index stream because the acceleration
// structure has one BLAS.  This keeps the object-to-source mapping here,
// rather than duplicating per-mesh tracing shaders.
import lib.scene_schema;
import uniforms;

/**
 * Object-space vertex transforms.
 *
 * These exist as functions rather than branches inside one entry point because
 * each one becomes a separate pipeline, and because everything here has to be
 * resolved into real geometry before a ray tracer can see it (section 5.1). A
 * compute pass will call the same functions to write deformed vertices into a
 * buffer, so they take their inputs explicitly rather than reading varyings.
 */

struct VertexSkin {
    float4 indices;
    float4 weights;
};

/** Rigid geometry - the object-space position passes through unchanged. */
float4 transformStatic(float3 position) {
    return float4(position, 1.0);
}

float4 skinPosition(float4 position, VertexSkin skin, bool previousFrame) {
    int i1 = max(0, int(skin.indices[0]));
    int i2 = max(0, int(skin.indices[1]));
    int i3 = max(0, int(skin.indices[2]));
    int i4 = max(0, int(skin.indices[3]));

    if (previousFrame) {
        return mul(boneUniforms.prevBones[i1], position) * skin.weights[0] +
               mul(boneUniforms.prevBones[i2], position) * skin.weights[1] +
               mul(boneUniforms.prevBones[i3], position) * skin.weights[2] +
               mul(boneUniforms.prevBones[i4], position) * skin.weights[3];
    }
    return mul(boneUniforms.bones[i1], position) * skin.weights[0] +
           mul(boneUniforms.bones[i2], position) * skin.weights[1] +
           mul(boneUniforms.bones[i3], position) * skin.weights[2] +
           mul(boneUniforms.bones[i4], position) * skin.weights[3];
}

/** Dangly vertices are simulated on the CPU and uploaded per frame. */
float4 danglyPosition(uint vertexId) {
    return danglyUniforms.positions[vertexId];
}

/**
 * Lightsaber blades are widened in the vertex stage from the vertex index. The
 * blade has 88 vertices per side, four per segment, 22 segments.
 */
float4 saberPosition(float3 position, uint vertexId) {
    float signum = 2.0 * (vertexId / 88) - 1.0;
    float hdist = ((vertexId % 88) / 4) / 21.0;
    float vdist = (vertexId % 4) / 3.0;
    return float4(position + 0.5 * signum * hdist * vdist * localUniforms.saberDisplacement.xyz, 1.0);
}

/**
 * Transforms a direction by the inverse-transpose of the model matrix. The
 * original spells this transpose(mat3(uModelInv)) * v; mul(v, M) is the
 * row-vector product, which is the same thing without building the transpose.
 */
float3 transformNormal(float3 n) {
    return mul(n, (float3x3)localUniforms.modelInv);
}

/** Tangent frame, carried as three vectors so no matrix convention is involved. */
struct TangentFrame {
    float3 tangent;
    float3 bitangent;
    float3 normal;
};

float3 tangentToWorld(TangentFrame frame, float3 n) {
    return frame.tangent * n.x + frame.bitangent * n.y + frame.normal * n.z;
}

[[vk::binding(0, 1)]] StructuredBuffer<SceneObject> objects;
[[vk::binding(1, 1)]] StructuredBuffer<Matrix3x4> bones;
[[vk::binding(2, 1)]] RWStructuredBuffer<MergedVertex> vertices;
[[vk::binding(3, 1)]] RWStructuredBuffer<uint> indices;
[[vk::binding(4, 1)]] RWStructuredBuffer<uint> materialIds;
[[vk::binding(5, 1)]] StructuredBuffer<float> sourceVertices;
[[vk::binding(6, 1)]] StructuredBuffer<uint> sourceIndices;

[[vk::binding(7, 1)]] StructuredBuffer<ProceduralQuad> proceduralQuads;
[[vk::binding(8, 1)]] StructuredBuffer<float4> danglyPositions;

[[vk::binding(9, 1)]] StructuredBuffer<GrassFace> grassFaces;
[[vk::binding(10, 1)]] StructuredBuffer<GrassRange> grassRanges;

struct PushConstants {
    uint objectCount;
    uint opaqueObjectCount;
    uint vertexCount;
    uint triangleCount;
    uint opaqueTriangleCount;
    float4 cameraPosition;
};

[[vk::push_constant]] PushConstants pushConstants;

float3 transformPoint(float4x4 matrix, float4 point) {
    return mul(matrix, point).xyz;
}

float3 transformNormal(float4x4 matrixInv, float3 normal) {
    return mul(normal, (float3x3)matrixInv);
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
float4 skinVertex(
    SceneObject object,
    float4 boneIndices,
    float4 boneWeights,
    float4 value,
    bool previous) {
    uint paletteBase = object.boneBase + (previous ? object.boneCount : 0);
    float4 result = float4(0);

    [unroll]
    for (uint influence = 0; influence < 4; ++influence) {
        uint bone = min(
            uint(max(0, int(boneIndices[influence]))),
            object.boneCount - 1);
        Matrix3x4 matrix = bones[paletteBase + bone];
        float4 transformed = float4(
            dot(matrix.row0, value),
            dot(matrix.row1, value),
            dot(matrix.row2, value),
            value.w);
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

GrassRange findGrassRange(SceneObject object, uint clusterId) {
    uint low = 0;
    uint high = object.srcIndexOffset;
    while (low + 1 < high) {
        uint middle = (low + high) / 2;
        if (grassRanges[object.srcVertexOffset + middle].clusterOffset <= clusterId) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return grassRanges[object.srcVertexOffset + low];
}

uint grassHash(uint faceIndex, uint clusterIndex, uint stream) {
    uint value = faceIndex * 0x9e3779b9u
        ^ clusterIndex * 0x85ebca6bu
        ^ stream * 0xc2b2ae35u;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float grassRandom01(uint faceIndex, uint clusterIndex, uint stream) {
    return float(grassHash(faceIndex, clusterIndex, stream) >> 8) / 16777216.0;
}

uint grassVariant(GrassFace face, uint faceIndex, uint clusterIndex) {
    float sum = face.probabilities.x + face.probabilities.y
        + face.probabilities.z + face.probabilities.w;
    float value = grassRandom01(faceIndex, clusterIndex, 2) * sum;
    float upper = 0.0;
    uint variant = 3;
    [unroll]
    for (uint i = 0; i < 3; ++i) {
        upper += face.probabilities[i];
        if (value < upper) {
            variant = i;
            break;
        }
    }
    return min(variant, max(1u, face.faceBudgetMaterialVariants.w) - 1u);
}

ProceduralQuad expandGrassCluster(SceneObject object, uint clusterId) {
    GrassRange range = findGrassRange(object, clusterId);
    GrassFace face = grassFaces[range.faceIndex];
    uint localCluster = clusterId - range.clusterOffset;
    uint sourceFace = face.faceBudgetMaterialVariants.x;
    float r1sqrt = sqrt(grassRandom01(sourceFace, localCluster, 0));
    float r2 = grassRandom01(sourceFace, localCluster, 1);
    float3 baryPosition = float3(
        1.0 - r1sqrt, r1sqrt * (1.0 - r2), r2 * r1sqrt);
    float3 position = face.vertex0Uv0x.xyz * baryPosition.x
        + face.vertex1Uv0y.xyz * baryPosition.y
        + face.vertex2Uv1x.xyz * baryPosition.z;
    float2 uv0 = float2(face.vertex0Uv0x.w, face.vertex1Uv0y.w);
    float2 uv1 = float2(face.vertex2Uv1x.w, face.uv1yUv2QuadSize.x);
    float2 uv2 = face.uv1yUv2QuadSize.yz;
    float2 lightmapUV = uv0 * baryPosition.x
        + uv1 * baryPosition.y + uv2 * baryPosition.z;
    uint variant = grassVariant(face, sourceFace, localCluster);
    float yaw = 6.28318530717958647692
        * grassRandom01(sourceFace, localCluster, 3);
    float sizeScale = saturate((32.0 - distance(position, pushConstants.cameraPosition.xyz)) / 4.0);
    // Live density: budgets are baked at the density cap and the dial rides in
    // cameraPosition.w as density/cap. Clusters beyond the active prefix emit
    // degenerate quads, the same mechanism the distance ring uses, so the
    // slider costs a push-constant change instead of a face-record rebuild.
    uint activeBudget = uint(round(
        float(face.faceBudgetMaterialVariants.y) * pushConstants.cameraPosition.w));
    if (localCluster >= activeBudget)
        sizeScale = 0.0;
    float quadSize = face.uv1yUv2QuadSize.w * sizeScale;

    ProceduralQuad cluster;
    cluster.positionVariant = float4(position, float(variant));
    cluster.right = float4(cos(yaw) * quadSize, sin(yaw) * quadSize, 0.0, 0.0);
    cluster.up = float4(0.0, 0.0, quadSize, 0.0);
    cluster.uvOffsetScale = float4(
        0.5 * float(variant % 2), 0.5 * float(variant / 2), 0.5, 0.5);
    cluster.lightmapUV = lightmapUV;
    cluster.pad = float2(0.0);
    cluster.color = float4(1.0);
    return cluster;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint id = tid.x;

    if (id < pushConstants.vertexCount) {
        SceneObject object = objects[findVertexObject(id)];
        uint vertexId = id - object.dstVertexBase;
        if (object.srcVertexStride <= 1) {
            ProceduralQuad cluster = object.srcVertexStride == 0
                ? expandGrassCluster(object, vertexId / 4)
                : proceduralQuads[object.srcVertexOffset + vertexId / 4];
            uint quadVertex = vertexId % 4;
            float x = (quadVertex == 0 || quadVertex == 3) ? -0.5 : 0.5;
            // Stride zero is the bottom-anchored grass mesh. Stride one is a
            // centered particle/billboard mesh, tagged by GpuScene so neither
            // needs thousands of CPU-side SceneObject records.
            float y = object.srcVertexStride == 0 ? (quadVertex < 2 ? 0.0 : 1.0)
                                                   : (quadVertex < 2 ? -0.5 : 0.5);
            MergedVertex mergedVertex;
            mergedVertex.position = cluster.positionVariant.xyz + cluster.right.xyz * x + cluster.up.xyz * y;
            mergedVertex.prevPosition = mergedVertex.position;
            mergedVertex.normal = safeNormalize(cross(cluster.right.xyz, cluster.up.xyz), float3(0, 0, 1));
            float2 uv = float2(x + 0.5, object.srcVertexStride == 0 ? y : y + 0.5);
            mergedVertex.uv1 = cluster.uvOffsetScale.xy + uv * cluster.uvOffsetScale.zw;
            mergedVertex.uv2 = cluster.lightmapUV;
            mergedVertex.bitangent = float3(0);
            mergedVertex.tangent = float3(0);
            mergedVertex.tanSpaceNormal = float3(0);
            mergedVertex.pad = float2(0);
            mergedVertex.color = cluster.color;
            vertices[id] = mergedVertex;
        } else {
        uint sourceVertex = object.srcVertexOffset + vertexId * (object.srcVertexStride / 4);
        float4 position = float4(
            sourceVertices[sourceVertex + object.offPosition / 4],
            sourceVertices[sourceVertex + object.offPosition / 4 + 1],
            sourceVertices[sourceVertex + object.offPosition / 4 + 2],
            1.0);
        float4 prevPosition = position;
        bool dangly = object.danglyBase != 0xffffffffu
            && object.danglyCount == object.vertexCount;
        if (dangly) {
            position = danglyPositions[object.danglyBase + vertexId];
            prevPosition = danglyPositions[object.danglyBase + object.danglyCount + vertexId];
        }
        float saberSignum = 2.0 * float(vertexId / 88) - 1.0;
        float saberHdist = float((vertexId % 88) / 4) / 21.0;
        float saberVdist = float(vertexId % 4) / 3.0;
        float3 saberOffset = 0.5 * saberSignum * saberHdist * saberVdist * object.saberDisplacement.xyz;
        position.xyz += saberOffset;
        prevPosition.xyz += saberOffset;
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
                position,
                false);
            prevPosition = skinVertex(
                object,
                boneIndices,
                boneWeights,
                prevPosition,
                true);
        }

        MergedVertex mergedVertex;
        mergedVertex.position = transformPoint(object.transform, position);
        mergedVertex.prevPosition = transformPoint(object.prevTransform, prevPosition);
        mergedVertex.color = float4(1.0);

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
                false).xyz;
        }

        mergedVertex.normal = safeNormalize(
            transformNormal(object.transformInv, normal),
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
                    false).xyz;
                tangent = skinVertex(
                    object,
                    boneIndices,
                    boneWeights,
                    float4(tangent, 0),
                    false).xyz;
                tanSpaceNormal = skinVertex(
                    object,
                    boneIndices,
                    boneWeights,
                    float4(tanSpaceNormal, 0),
                    false).xyz;
            }

            mergedVertex.bitangent = safeNormalize(
                transformNormal(object.transformInv, bitangent),
                float3(0, 1, 0));
            mergedVertex.tangent = safeNormalize(
                transformNormal(object.transformInv, tangent),
                float3(1, 0, 0));
            mergedVertex.tanSpaceNormal = safeNormalize(
                transformNormal(object.transformInv, tanSpaceNormal),
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
        if (object.srcVertexStride <= 1) {
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
        if (object.srcVertexStride == 0) {
            GrassRange range = findGrassRange(object, localId / 2);
            materialIds[id] = grassFaces[range.faceIndex].faceBudgetMaterialVariants.z;
        } else {
            materialIds[id] = object.materialIndex;
        }
    }
}
