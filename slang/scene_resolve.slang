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
[[vk::binding(11, 1)]] StructuredBuffer<float4> grassCardVertices;
[[vk::binding(12, 1)]] StructuredBuffer<uint> grassCardIndices;
[[vk::binding(13, 1)]] RWStructuredBuffer<GrassCardInstance> grassCardInstances;

struct PushConstants {
    uint objectCount;
    uint opaqueObjectCount;
    uint vertexCount;
    uint triangleCount;
    uint opaqueTriangleCount;
    float grassCardAspect;
    uint grassCardCount;
    uint pad1;
    /** xyz camera. */
    float4 cameraPosition;
    /** Mirrors GrassParams in graphics/rendering/gpuscene.h. */
    float grassRadius;
    float grassCurvature;
    float grassCurvatureVariance;
    float grassSparsity;
    float grassDisplacement;
    float grassLength;
    float grassLengthVariance;
    float grassWidth;
    float grassYOffset;
    float grassRoughness;
    uint grassBladesPerCluster;
    uint grassBudgetBlades;
    float grassDensity;
    float grassOrientation;
    float grassOrientationVariance;
    uint grassPad0;
    float4 grassColor;
    float grassWindStrength;
    float grassWindDirection;
    float grassWindSpeed;
    float grassWindWavelength;
    float grassWindGust;
    uint grassCardVerts;
    uint grassCardTris;
    uint grassPad1;
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

// bones[boneBase + b] is the current palette, bones[boneBase + boneCount + b]
// the previous; one helper so position and frame skinning cannot drift.
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

uint findCardObject(uint id) {
    for (uint object = 0; object < pushConstants.objectCount; ++object) {
        uint first = objects[object].dstCardBase;
        if (id >= first && id - first < objects[object].cardCount) return object;
    }
    return 0;
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

/**
 * Vertices and triangles per blade: five pairs up the blade and a tip vertex,
 * nine triangles. Four segments read as a curve; the tip is a real triangle,
 * since a zero-area one has no geometric normal.
 */
/** One blade, positioned and oriented; the vertices are derived from it. */
struct GrassBlade {
    float3 root;
    /** Horizontal direction the blade bends toward, and its perpendicular. */
    float3 bendDir;
    float3 widthDir;
    float length;
    float width;
    float curvature;
    float2 lightmapUV;
    /** Per-blade phase offset, so neighbours do not sway as one sheet. */
    float windPhase;
    uint variant;
    bool culled;
};

GrassBlade expandGrassBlade(SceneObject object, uint bladeId, bool card) {
    // Several blades share a cluster, so the cluster is what the face records
    // and the range lookup know about; the sub-index only ever reaches the
    // hash, where it separates siblings that would otherwise be identical.
    uint perCluster = max(1u, pushConstants.grassBladesPerCluster);
    uint clusterId = bladeId / perCluster;
    uint sibling = bladeId % perCluster;
    GrassRange range = findGrassRange(object, clusterId);
    GrassFace face = grassFaces[range.faceIndex];
    uint localCluster = (clusterId - range.clusterOffset) * perCluster + sibling;
    uint sourceFace = face.faceBudgetMaterialVariants.x;

    // Barycentric on the source triangle, as before: an even scatter over the
    // face rather than over its bounding box.
    float r1sqrt = sqrt(grassRandom01(sourceFace, localCluster, 0));
    float r2 = grassRandom01(sourceFace, localCluster, 1);
    float3 bary = float3(1.0 - r1sqrt, r1sqrt * (1.0 - r2), r2 * r1sqrt);
    float3 position = face.vertex0Uv0x.xyz * bary.x
        + face.vertex1Uv0y.xyz * bary.y
        + face.vertex2Uv1x.xyz * bary.z;
    float2 uv0 = float2(face.vertex0Uv0x.w, face.vertex1Uv0y.w);
    float2 uv1 = float2(face.vertex2Uv1x.w, face.uv1yUv2QuadSize.x);
    float2 uv2 = face.uv1yUv2QuadSize.yz;

    GrassBlade blade;
    blade.lightmapUV = uv0 * bary.x + uv1 * bary.y + uv2 * bary.z;

    float quadSize = face.uv1yUv2QuadSize.w;
    // Displacement rides on the authored quad size, which is the only length
    // scale a face carries; a cell is one of those across.
    float2 jitter = float2(grassRandom01(sourceFace, localCluster, 4),
                           grassRandom01(sourceFace, localCluster, 5)) * 2.0 - 1.0;
    // The jitter is horizontal, so on a slope it walks the root off the face;
    // following the face plane keeps it on the ground, for strands and cards.
    float2 displacement = jitter * pushConstants.grassDisplacement * quadSize;
    float3 supportNormal = cross(face.vertex1Uv0y.xyz - face.vertex0Uv0x.xyz,
                                 face.vertex2Uv1x.xyz - face.vertex0Uv0x.xyz);
    position.xy += displacement;
    if (abs(supportNormal.z) > 1e-5)
        position.z -= dot(supportNormal.xy, displacement) / supportNormal.z;

    // Centred on the orientation rather than starting from it, so narrowing
    // the variation closes in on the angle asked for instead of sweeping away
    // from it. A full turn of variation is the same uniform scatter as before.
    float yaw = pushConstants.grassOrientation +
        pushConstants.grassOrientationVariance *
            (grassRandom01(sourceFace, localCluster, 3) - 0.5);
    blade.bendDir = float3(cos(yaw), sin(yaw), 0.0);
    blade.widthDir = float3(-sin(yaw), cos(yaw), 0.0);
    // widthDir lies in the face plane, or a sloped face lifts one end of the base.
    if (abs(supportNormal.z) > 1e-5) {
        float3 tangentWidth = blade.widthDir;
        tangentWidth.z = -dot(supportNormal.xy, tangentWidth.xy) / supportNormal.z;
        blade.widthDir = safeNormalize(tangentWidth, blade.widthDir);
    }

    float lengthJitter = 1.0 + pushConstants.grassLengthVariance
        * (grassRandom01(sourceFace, localCluster, 6) * 2.0 - 1.0);
    blade.length = quadSize * pushConstants.grassLength * max(0.0, lengthJitter);
    // grassWidth is a strand dial; grassCardAspect is the card counterpart.
    // A card depicts a tuft and needs to be broad at its default, while a
    // strand remains a thin ribbon, so the two cannot share a default.
    blade.width = blade.length * (card ? pushConstants.grassCardAspect
                                       : pushConstants.grassWidth);
    blade.curvature = pushConstants.grassCurvature + pushConstants.grassCurvatureVariance
        * (grassRandom01(sourceFace, localCluster, 7) * 2.0 - 1.0);
    blade.windPhase = 6.28318530717958647692 * grassRandom01(sourceFace, localCluster, 9);
    if (card)
        blade.variant = grassVariant(face, sourceFace, localCluster);

    // Grown in over the last part of the radius: a face is admitted whole, so
    // switching on would advance the field in chunks.
    float distanceToCamera = distance(position, pushConstants.cameraPosition.xyz);
    float fadeBand = max(1.0, pushConstants.grassRadius * 0.2);
    float fade = saturate((pushConstants.grassRadius - distanceToCamera) / fadeBand);
    blade.length *= fade;
    blade.width *= fade;

    position.z += blade.length * pushConstants.grassYOffset;
    blade.root = position;

    // Sparsity is a hash, so which blades are missing is fixed frame to frame.
    // Which clusters exist at all was decided on the CPU; every one here was chosen.
    bool sparse = grassRandom01(sourceFace, localCluster, 8) < pushConstants.grassSparsity;
    blade.culled = sparse;
    return blade;
}

/**
 * Wind at a point and a time: a fast short rustle wave and a slow long gust
 * wave along the wind direction. A field of position and time only, so two
 * evaluations give a true motion vector; the per-blade phase keeps neighbours
 * from moving as one sheet.
 */
float grassWindBend(float3 position, float phase, float time) {
    float2 windDir = float2(cos(pushConstants.grassWindDirection),
                            sin(pushConstants.grassWindDirection));
    float along = dot(position.xy, windDir);
    float k = 6.28318530717958647692 / max(0.01, pushConstants.grassWindWavelength);
    float rustle = sin(k * along - pushConstants.grassWindSpeed * time * k + phase);
    // A quarter the frequency and a third the speed: the gust is the thing you
    // watch cross the field, the rustle is what the blade does while it waits.
    float gust = sin(0.25 * k * along - 0.33 * pushConstants.grassWindSpeed * time * k
                     + phase * 0.5);
    float mixed = lerp(rustle, gust, pushConstants.grassWindGust);
    // Wind pushes; it does not pull the blade upwind. Biased to [0,1] so the
    // rest position is the unbent blade rather than the average of a sway.
    return pushConstants.grassWindStrength * (0.5 + 0.5 * mixed);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint id = tid.x;

    if (id < pushConstants.grassCardCount) {
        SceneObject object = objects[findCardObject(id)];
        GrassBlade card = expandGrassBlade(object, id - object.dstCardBase, true);
        float bendNow = card.curvature + grassWindBend(
            card.root, card.windPhase, globalUniforms.time);
        float bendPrev = card.curvature + grassWindBend(
            card.root, card.windPhase, globalUniforms.prevTime);
        float3 width = card.culled ? float3(0) : card.widthDir * card.width;
        float3 upNow = card.culled ? float3(0)
            : float3(0, 0, card.length) + card.bendDir * (bendNow * card.length);
        float3 upPrev = card.culled ? float3(0)
            : float3(0, 0, card.length) + card.bendDir * (bendPrev * card.length);
        GrassCardInstance instance;
        instance.row0 = float4(width.x, upNow.x, 0, card.root.x);
        instance.row1 = float4(width.y, upNow.y, 0, card.root.y);
        instance.row2 = float4(width.z, upNow.z, 0, card.root.z);
        instance.prevRow0 = float4(width.x, upPrev.x, 0, card.root.x);
        instance.prevRow1 = float4(width.y, upPrev.y, 0, card.root.y);
        instance.prevRow2 = float4(width.z, upPrev.z, 0, card.root.z);
        instance.lightmapVariantCulled = float4(
            card.lightmapUV, float(card.variant), card.culled ? 1.0 : 0.0);
        instance.materialIndex = uint4(object.materialIndex, 0, 0, 0);
        grassCardInstances[id] = instance;
    }

    if (id < pushConstants.vertexCount) {
        SceneObject object = objects[findVertexObject(id)];
        uint vertexId = id - object.dstVertexBase;
        // Strands are the only primitive here that is not four vertices, so
        // the grass branch has to come before the shared quad path rather than
        // inside it.
        if (object.srcVertexStride == 0) {
            uint cardVerts = pushConstants.grassCardVerts;
            uint cardId = vertexId / cardVerts;
            uint local = vertexId % cardVerts;
            GrassCardInstance card = grassCardInstances[object.dstCardBase + cardId];
            uint variant = uint(card.lightmapVariantCulled.z);
            bool culled = card.lightmapVariantCulled.w != 0.0;
            float4 tv = grassCardVertices[variant * cardVerts + local];
            float4 templateVertex = float4(tv.x, tv.y, 0, 1);
            float3 position = float3(dot(card.row0, templateVertex), dot(card.row1, templateVertex), dot(card.row2, templateVertex));
            float3 prevPosition = float3(dot(card.prevRow0, templateVertex), dot(card.prevRow1, templateVertex), dot(card.prevRow2, templateVertex));
            MergedVertex mergedVertex;
            mergedVertex.position = position;
            mergedVertex.prevPosition = prevPosition;
            float3 normal = safeNormalize(cross(float3(card.row0.y, card.row1.y, card.row2.y), float3(card.row0.x, card.row1.x, card.row2.x)), float3(0, 0, 1));
            mergedVertex.normal = culled || dot(normal, pushConstants.cameraPosition.xyz - position) < 0.0 ? -normal : normal;
            mergedVertex.uv1 = float2(0.5 * float(variant % 2), 0.5 * float(variant / 2)) + tv.zw * 0.5;
            mergedVertex.uv2 = card.lightmapVariantCulled.xy;
            mergedVertex.bitangent = float3(0);
            mergedVertex.tangent = float3(0);
            mergedVertex.tanSpaceNormal = float3(0);
            mergedVertex.pad = float2(0);
            mergedVertex.color = float4(1.0);
            vertices[id] = mergedVertex;
        } else if (object.srcVertexStride <= 1) {
            ProceduralQuad cluster = proceduralQuads[object.srcVertexOffset + vertexId / 4];
            uint quadVertex = vertexId % 4;
            float x = (quadVertex == 0 || quadVertex == 3) ? -0.5 : 0.5;
            float y = quadVertex < 2 ? -0.5 : 0.5;
            MergedVertex mergedVertex;
            mergedVertex.position = cluster.positionVariant.xyz + cluster.right.xyz * x + cluster.up.xyz * y;
            mergedVertex.prevPosition = mergedVertex.position;
            mergedVertex.normal = safeNormalize(cross(cluster.right.xyz, cluster.up.xyz), float3(0, 0, 1));
            float2 uv = float2(x + 0.5, y + 0.5);
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
        bool dangly = object.danglyBase != 0xffffffffu;
        if (dangly) {
            position = danglyPositions[object.danglyBase + vertexId];
            prevPosition = danglyPositions[object.danglyBase + object.vertexCount + vertexId];
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
        if (object.srcVertexStride == 0) {
            uint cardTris = pushConstants.grassCardTris;
            uint cardVerts = pushConstants.grassCardVerts;
            uint cardId = localId / cardTris;
            uint tri = localId % cardTris;
            GrassCardInstance card = grassCardInstances[object.dstCardBase + cardId];
            uint variant = uint(card.lightmapVariantCulled.z);
            uint base = object.dstVertexBase + cardId * cardVerts;
            uint ibase = variant * cardTris * 3 + tri * 3;
            indices[destinationIndex] = base + grassCardIndices[ibase];
            indices[destinationIndex + 1] = base + grassCardIndices[ibase + 1];
            indices[destinationIndex + 2] = base + grassCardIndices[ibase + 2];
        } else if (object.srcVertexStride <= 1) {
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
            uint perBlade = pushConstants.grassCardTris *
                            max(1u, pushConstants.grassBladesPerCluster);
            GrassRange range = findGrassRange(object, localId / perBlade);
            materialIds[id] = grassFaces[range.faceIndex].faceBudgetMaterialVariants.z;
        } else {
            materialIds[id] = object.materialIndex;
        }
    }
}
