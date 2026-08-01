/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace reone::graphics {

class Mesh;

struct alignas(16) GpuSceneMaterial {
    glm::vec4 selfIllumColor {0.0f};
    glm::vec4 diffuseColor {1.0f};
    glm::vec4 uv0 {1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 uv1 {0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 uv2 {0.0f};
    uint32_t mainTex {UINT32_MAX};
    uint32_t normalMap {UINT32_MAX};
    uint32_t lightmap {UINT32_MAX};
    uint32_t bumpMapArray {UINT32_MAX};
    uint32_t featureMask {0};
    int32_t bumpMapFrame {0};
    float bumpMapScale {1.0f};
    float curatedPad[1] {};
    glm::vec4 curatedAlbedoMul {1.0f, 1.0f, 1.0f, 0.0f};
    glm::vec4 curatedRoughA {0.0f};
    glm::vec4 curatedRoughB {0.0f};
    glm::vec4 curatedMetalA {0.0f};
    glm::vec4 curatedMetalB {0.0f};
    glm::vec4 curatedEmission {0.0f};
    uint32_t surfaceType {0};
    float roughnessScale {1.0f};
    float overridePad[2] {};
    glm::vec4 overrideColor {1.0f, 1.0f, 1.0f, 0.0f};
    glm::vec4 overrideParams {-1.0f, 1.0f, 1.0f, 1.0f};
};
static_assert(offsetof(GpuSceneMaterial, mainTex) == 80);
static_assert(offsetof(GpuSceneMaterial, curatedAlbedoMul) == 112);
static_assert(offsetof(GpuSceneMaterial, curatedEmission) == 192);
static_assert(offsetof(GpuSceneMaterial, surfaceType) == 208);
static_assert(offsetof(GpuSceneMaterial, roughnessScale) == 212);
static_assert(offsetof(GpuSceneMaterial, overrideColor) == 224);
static_assert(offsetof(GpuSceneMaterial, overrideParams) == 240);
static_assert(sizeof(GpuSceneMaterial) == 256);

/** Three row vectors encode a float3x4 exactly as skin.slang reads it. */
struct alignas(16) GpuSceneMatrix3x4 {
    glm::vec4 row0 {1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 row1 {0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 row2 {0.0f, 0.0f, 1.0f, 0.0f};
};
static_assert(sizeof(GpuSceneMatrix3x4) == 48);

/** std430-compatible canonical vertex used by skin.slang and consumers. */
struct alignas(16) GpuSceneMergedVertex {
    glm::vec3 position {0.0f};
    float positionPad {0.0f};
    glm::vec3 normal {0.0f};
    float normalPad {0.0f};
    glm::vec2 uv1 {0.0f};
    glm::vec2 uv2 {0.0f};
    glm::vec3 tangent {0.0f};
    float tangentPad {0.0f};
    glm::vec3 bitangent {0.0f};
    float bitangentPad {0.0f};
    glm::vec3 tanSpaceNormal {0.0f};
    float tanSpaceNormalPad {0.0f};
    glm::vec3 prevPosition {0.0f};
    float prevPositionPad {0.0f};
    glm::vec2 pad {0.0f};
    float tailPad[2] {};
    glm::vec4 color {1.0f};
    glm::vec3 objectPosition {0.0f};
    float objectPositionPad {0.0f};
};
static_assert(offsetof(GpuSceneMergedVertex, position) == 0);
static_assert(offsetof(GpuSceneMergedVertex, normal) == 16);
static_assert(offsetof(GpuSceneMergedVertex, uv1) == 32);
static_assert(offsetof(GpuSceneMergedVertex, uv2) == 40);
static_assert(offsetof(GpuSceneMergedVertex, tangent) == 48);
static_assert(offsetof(GpuSceneMergedVertex, bitangent) == 64);
static_assert(offsetof(GpuSceneMergedVertex, tanSpaceNormal) == 80);
static_assert(offsetof(GpuSceneMergedVertex, prevPosition) == 96);
static_assert(offsetof(GpuSceneMergedVertex, pad) == 112);
static_assert(offsetof(GpuSceneMergedVertex, color) == 128);
static_assert(offsetof(GpuSceneMergedVertex, objectPosition) == 144);
static_assert(sizeof(GpuSceneMergedVertex) == 160);

struct alignas(16) GpuSceneObjectData {
    glm::mat4 transform {1.0f};
    glm::mat4 prevTransform {1.0f};
    glm::mat4 transformInv {1.0f};
    uint32_t srcVertexOffset {0};
    uint32_t srcIndexOffset {0};
    uint32_t srcVertexStride {0};
    int32_t offPosition {-1};
    int32_t offNormals {-1};
    int32_t offUV1 {-1};
    int32_t offUV2 {-1};
    int32_t offTanSpace {-1};
    int32_t offBoneIndices {-1};
    int32_t offBoneWeights {-1};
    uint32_t vertexCount {0};
    uint32_t triangleCount {0};
    uint32_t dstVertexBase {0};
    uint32_t dstTriangleBase {0};
    uint32_t geometryIndex {0};
    uint32_t boneBase {UINT32_MAX};
    uint32_t boneCount {0};
    uint32_t materialIndex {0};
    uint32_t danglyBase {UINT32_MAX};
    uint32_t danglyCount {0};
    alignas(16) glm::vec4 saberDisplacement {0.0f};
};
static_assert(offsetof(GpuSceneObjectData, srcVertexOffset) == 192);
static_assert(offsetof(GpuSceneObjectData, srcIndexOffset) == 196);
static_assert(offsetof(GpuSceneObjectData, srcVertexStride) == 200);
static_assert(offsetof(GpuSceneObjectData, offPosition) == 204);
static_assert(offsetof(GpuSceneObjectData, vertexCount) == 232);
static_assert(offsetof(GpuSceneObjectData, materialIndex) == 260);
static_assert(offsetof(GpuSceneObjectData, danglyBase) == 264);
static_assert(offsetof(GpuSceneObjectData, saberDisplacement) == 272);
static_assert(sizeof(GpuSceneObjectData) == 288);

struct alignas(16) GpuSceneProceduralQuad {
    glm::vec4 positionVariant {0.0f};
    glm::vec4 right {0.0f};
    glm::vec4 up {0.0f};
    glm::vec4 uvOffsetScale {0.0f, 0.0f, 1.0f, 1.0f};
    glm::vec2 lightmapUV {0.0f};
    glm::vec2 pad {0.0f};
    glm::vec4 color {1.0f};
};
static_assert(sizeof(GpuSceneProceduralQuad) == sizeof(glm::vec4) * 6);

enum class GpuScenePrimitiveClass { Opaque,
                                    NonOpaque };
enum class GpuSceneResidencyClass { Static,
                                    Dynamic };

struct GpuSceneObjectInput {
    GpuSceneObjectData data;
    const Mesh *sourceMesh {nullptr};
    uint32_t objectIndex {0};
    uint32_t objectGeneration {0};
};

/** Vulkan-free output of scene admission and consumer classification. */
struct GpuSceneUpload {
    std::vector<GpuSceneMaterial> materials;
    std::vector<GpuSceneObjectInput> objects;
    std::vector<GpuSceneMatrix3x4> bones;
    std::vector<glm::vec4> danglyPositions;
    std::vector<GpuSceneProceduralQuad> proceduralQuads;
    uint32_t opaqueObjectCount {0};
};

} // namespace reone::graphics
