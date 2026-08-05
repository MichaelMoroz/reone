/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <array>

#include <memory>

#include <unordered_map>

#include "reone/graphics/commandbuffer.h"

#include "reone/graphics/gpuscenecontext.h"

#include <glm/glm.hpp>

namespace reone::graphics {

class Mesh;

// Mirrors of the GPU scene tables. Each one carries the *same name* as its
// declaration in slang/lib/scene_schema.slang, because the startup reflection
// check pairs them by name; types below with a GpuScene prefix have no Slang
// counterpart and exist only on the CPU side of admission.

struct alignas(16) InstanceMaterial {
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
    uint32_t envMap {UINT32_MAX};
    glm::vec4 curatedAlbedoMul {1.0f, 1.0f, 1.0f, 0.0f};
    glm::vec4 curatedRoughA {0.0f};
    glm::vec4 curatedRoughB {0.0f};
    glm::vec4 curatedMetalA {0.0f};
    glm::vec4 curatedMetalB {0.0f};
    glm::vec4 curatedEmission {0.0f};
    uint32_t surfaceType {0};
    float roughnessScale {1.0f};
    uint32_t envMapCube {UINT32_MAX};
    float waterAlpha {1.0f};
    glm::vec4 overrideColor {1.0f, 1.0f, 1.0f, 0.0f};
    glm::vec4 overrideParams {-1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 ambientColor {1.0f};
    int32_t envMapDerivedLayer {0};
    float tailPad[3] {};
};
static_assert(offsetof(InstanceMaterial, mainTex) == 80);
static_assert(offsetof(InstanceMaterial, envMap) == 108);
static_assert(offsetof(InstanceMaterial, curatedAlbedoMul) == 112);
static_assert(offsetof(InstanceMaterial, curatedEmission) == 192);
static_assert(offsetof(InstanceMaterial, surfaceType) == 208);
static_assert(offsetof(InstanceMaterial, roughnessScale) == 212);
static_assert(offsetof(InstanceMaterial, envMapCube) == 216);
static_assert(offsetof(InstanceMaterial, waterAlpha) == 220);
static_assert(offsetof(InstanceMaterial, overrideColor) == 224);
static_assert(offsetof(InstanceMaterial, overrideParams) == 240);
static_assert(offsetof(InstanceMaterial, ambientColor) == 256);
static_assert(offsetof(InstanceMaterial, envMapDerivedLayer) == 272);
static_assert(sizeof(InstanceMaterial) == 288);

/** Three row vectors encode a float3x4 exactly as skin.slang reads it. */
struct alignas(16) Matrix3x4 {
    glm::vec4 row0 {1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 row1 {0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 row2 {0.0f, 0.0f, 1.0f, 0.0f};
};
static_assert(sizeof(Matrix3x4) == 48);

/** std430-compatible canonical vertex used by skin.slang and consumers. */
struct alignas(16) MergedVertex {
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
};
static_assert(offsetof(MergedVertex, position) == 0);
static_assert(offsetof(MergedVertex, normal) == 16);
static_assert(offsetof(MergedVertex, uv1) == 32);
static_assert(offsetof(MergedVertex, uv2) == 40);
static_assert(offsetof(MergedVertex, tangent) == 48);
static_assert(offsetof(MergedVertex, bitangent) == 64);
static_assert(offsetof(MergedVertex, tanSpaceNormal) == 80);
static_assert(offsetof(MergedVertex, prevPosition) == 96);
static_assert(offsetof(MergedVertex, pad) == 112);
static_assert(offsetof(MergedVertex, color) == 128);
static_assert(sizeof(MergedVertex) == 144);

struct alignas(16) SceneObject {
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
static_assert(offsetof(SceneObject, srcVertexOffset) == 192);
static_assert(offsetof(SceneObject, srcIndexOffset) == 196);
static_assert(offsetof(SceneObject, srcVertexStride) == 200);
static_assert(offsetof(SceneObject, offPosition) == 204);
static_assert(offsetof(SceneObject, vertexCount) == 232);
static_assert(offsetof(SceneObject, materialIndex) == 260);
static_assert(offsetof(SceneObject, danglyBase) == 264);
static_assert(offsetof(SceneObject, saberDisplacement) == 272);
static_assert(sizeof(SceneObject) == 288);

struct alignas(16) ProceduralQuad {
    glm::vec4 positionVariant {0.0f};
    glm::vec4 right {0.0f};
    glm::vec4 up {0.0f};
    glm::vec4 uvOffsetScale {0.0f, 0.0f, 1.0f, 1.0f};
    glm::vec2 lightmapUV {0.0f};
    glm::vec2 pad {0.0f};
    glm::vec4 color {1.0f};
};
static_assert(sizeof(ProceduralQuad) == sizeof(glm::vec4) * 6);

/**
 * Persistent input for one authored grass face. The triangle is already in
 * world space. UV components are packed into otherwise unused fourth lanes so
 * the merge shader can read one cache-line-aligned record without a second
 * source stream.
 */
struct alignas(16) GrassFace {
    glm::vec4 vertex0Uv0x {0.0f};
    glm::vec4 vertex1Uv0y {0.0f};
    glm::vec4 vertex2Uv1x {0.0f};
    glm::vec4 uv1yUv2QuadSize {0.0f};
    glm::vec4 probabilities {0.0f};
    glm::vec4 boundsMin {0.0f};
    glm::vec4 boundsMax {0.0f};
    glm::uvec4 faceBudgetMaterialVariants {0u};
};
static_assert(sizeof(GrassFace) == sizeof(glm::vec4) * 8);

/** Per-frame prefix-sum entry selecting a persistent grass face. */
struct alignas(16) GrassRange {
    uint32_t faceIndex {0};
    uint32_t clusterOffset {0};
    uint32_t clusterCount {0};
    uint32_t pad {0};
};
static_assert(sizeof(GrassRange) == sizeof(glm::uvec4));

enum class GpuScenePrimitiveClass { Opaque,
                                    NonOpaque };
enum class GpuSceneResidencyClass { Static,
                                    Dynamic };

struct GpuSceneObjectInput {
    SceneObject data;
    const Mesh *sourceMesh {nullptr};
    uint32_t objectIndex {0};
    uint32_t objectGeneration {0};
};

/** Vulkan-free output of shared scene admission. */
struct GpuSceneUpload {
    std::vector<InstanceMaterial> materials;
    std::vector<GpuSceneObjectInput> objects;
    std::vector<Matrix3x4> bones;
    std::vector<glm::vec4> danglyPositions;
    std::vector<ProceduralQuad> proceduralQuads;
    std::vector<GrassFace> grassFaces;
    std::vector<GrassRange> grassRanges;
    glm::vec4 cameraPosition {0.0f, 0.0f, 0.0f, 1.0f};
    uint64_t grassFaceGeneration {0};
    uint32_t opaqueObjectCount {0};
    uint32_t materialReferenceCount {0};
};

class GpuScene : boost::noncopyable {
public:
    using BufferView = graphics::BufferView;
    struct PrimitiveId {
        uint64_t sceneScope {0};
        uint32_t objectIndex {0};
        uint32_t objectGeneration {0};
        uint32_t localPrimitive {0};
    };
    struct PrimitiveIdRange {
        uint32_t firstTriangle {0};
        uint32_t triangleCount {0};
        PrimitiveId first;
    };
    struct PrimitiveIdView {
        const PrimitiveIdRange *ranges {nullptr};
        uint32_t rangeCount {0};
        PrimitiveId operator[](uint32_t index) const;
    };
    struct Region {
        GpuSceneResidencyClass residency {GpuSceneResidencyClass::Dynamic};
        uint64_t revision {0};
        uint32_t firstVertex {0};
        uint32_t vertexCount {0};
        uint32_t firstTriangle {0};
        uint32_t triangleCount {0};
    };
    struct View {
        uint64_t sceneScope {0};
        uint64_t revision {0};
        BufferView vertices;
        BufferView indices;
        BufferView materialIds;
        BufferView materials;
        uint32_t objectCount {0};
        uint32_t opaqueObjectCount {0};
        uint32_t vertexCount {0};
        uint32_t opaqueTriangleCount {0};
        uint32_t triangleCount {0};
        PrimitiveIdView primitiveIds;
        std::vector<Region> regions;
    };

    GpuScene();
    ~GpuScene();

    void init(IGpuSceneContext &context);
    void deinit();
    View update(ICommandBuffer &commandBuffer, GpuSceneUpload &upload);

private:
    struct Frame;
    struct SourceGeometry {
        uint32_t vertexOffset {0};
        uint32_t indexOffset {0};
        uint32_t vertexDataCount {0};
        uint32_t indexCount {0};
    };

    IGpuSceneContext *_context {nullptr};
    std::unique_ptr<IGpuSceneMergePipeline> _mergePipeline;
    std::array<std::unique_ptr<Frame>, 2> _frames;
    std::unordered_map<const Mesh *, SourceGeometry> _sourceGeometry;
    std::vector<float> _sourceVertexData;
    std::vector<uint32_t> _sourceIndexData;
    std::unique_ptr<IBuffer> _sourceVertices;
    std::unique_ptr<IBuffer> _sourceIndices;
    std::unique_ptr<IBuffer> _grassFaces;
    std::vector<std::unique_ptr<IBuffer>> _retiredSourceBuffers;
    uint32_t _sourceVertexCapacity {0};
    uint32_t _sourceIndexCapacity {0};
    uint64_t _grassFaceGeneration {0};
    uint64_t _sourceResourceGeneration {0};
    uint64_t _sceneScope {1};
    uint64_t _revision {0};
    bool _inited {false};

    void ensureMergeBuffers(Frame &, uint32_t, uint32_t, uint32_t, uint32_t,
                            uint32_t, uint32_t, uint32_t);
    void clearSourceGeometry();
    const SourceGeometry &appendSourceGeometry(const Mesh &mesh);
};

} // namespace reone::graphics
