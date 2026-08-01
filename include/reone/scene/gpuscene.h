/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <volk.h>

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <glm/glm.hpp>

#include "reone/graphics/frustum.h"
#include "reone/graphics/material.h"
#include "reone/scene/node.h"
#include "reone/scene/render/pipeline/tracematerials.h"

namespace reone::graphics {
class Mesh;
class Texture;
class VulkanBuffer;
class VulkanRenderer;
} // namespace reone::graphics
namespace reone::scene {
class CameraSceneNode;
class IRenderPassExecutor;
class ModelSceneNode;

enum class RenderPassName {
    None,
    DirLightShadowsPass,
    PointLightShadows,
    OpaqueGeometry,
    TransparentGeometry,
    PostProcessing,
};

using RenderPassFlags = uint32_t;
constexpr RenderPassFlags renderPassFlag(RenderPassName pass) {
    return pass == RenderPassName::None ? 0 : 1u << (static_cast<uint32_t>(pass) - 1);
}

enum class RenderCategory : uint32_t {
    None = 0,
    ShadowCaster = 1 << 0,
    Opaque = 1 << 1,
    Transparent = 1 << 2,
    LensFlare = 1 << 3,
};
using RenderCategories = uint32_t;
constexpr RenderCategories renderCategory(RenderCategory category) {
    return static_cast<RenderCategories>(category);
}

struct RenderFilter {
    RenderPassName pass {RenderPassName::None};
    RenderCategory category {RenderCategory::None};
};

enum class VisibilityPolicyKind { ViewCamera,
                                  Frusta,
                                  None };
struct VisibilityPolicy {
    VisibilityPolicyKind kind {VisibilityPolicyKind::ViewCamera};
    const CameraSceneNode *drawDistanceCamera {nullptr};
    const graphics::Frustum *lightFrusta {nullptr};
    size_t numLightFrusta {0};

    static VisibilityPolicy viewCamera(const CameraSceneNode *camera) {
        return {VisibilityPolicyKind::ViewCamera, camera, nullptr, 0};
    }
    static VisibilityPolicy shadowFrusta(const graphics::Frustum *frusta, size_t numFrusta,
                                         const CameraSceneNode *drawDistanceCamera) {
        return {VisibilityPolicyKind::Frusta, drawDistanceCamera, frusta, numFrusta};
    }
    static VisibilityPolicy noCulling() {
        return {VisibilityPolicyKind::None, nullptr, nullptr, 0};
    }
};

struct ParticleInstance {
    int frame {0};
    glm::vec3 position {0.0f};
    glm::vec2 size {0.0f};
    glm::vec4 color {1.0f};
    glm::vec3 right {0.0f};
    glm::vec3 up {0.0f};
};
struct GrassInstance {
    int variant {0};
    glm::vec3 position {0.0f};
    glm::vec2 lightmapUV {0.0f};
    float yaw {0.0f};
};
struct RegisteredSkin {
    std::vector<glm::mat4> bones;
    std::vector<glm::mat4> prevBones;
};
struct RegisteredDangly {
    std::vector<glm::vec4> positions;
    std::vector<glm::vec4> prevPositions;
};
struct RegisteredSaber {
    glm::vec4 displacement {0.0f};
};
using RegisteredDeformation =
    std::variant<std::monostate, RegisteredSkin, RegisteredDangly, RegisteredSaber>;

struct RegisteredMesh {
    RenderCategories categories {0};
    SceneNodeId id;
    SceneNodeNameIds nameIds;
    RenderPassFlags drawnPasses {0};
    std::reference_wrapper<graphics::Mesh> mesh;
    graphics::Material material;
    glm::mat4 transform {1.0f};
    glm::mat4 transformInv {1.0f};
    glm::mat4 prevTransform {1.0f};
    RegisteredDeformation deformation;
    ModelSceneNode *cullRoot {nullptr};
};
struct RegisteredBillboard {
    RenderCategories categories {0};
    SceneNodeId id;
    SceneNodeNameIds nameIds;
    RenderPassFlags drawnPasses {0};
    std::reference_wrapper<graphics::Texture> texture;
    glm::vec4 color {1.0f};
    glm::mat4 transform {1.0f};
    glm::mat4 transformInv {1.0f};
    std::optional<float> size;
    ModelSceneNode *cullRoot {nullptr};
};
struct RegisteredParticles {
    RenderCategories categories {0};
    SceneNodeId id;
    SceneNodeNameIds nameIds;
    RenderPassFlags drawnPasses {0};
    graphics::Material material;
    glm::ivec2 gridSize {1};
    std::vector<ParticleInstance> instances;
    ModelSceneNode *cullRoot {nullptr};
};
struct RegisteredGrass {
    RenderCategories categories {0};
    SceneNodeId id;
    SceneNodeNameIds nameIds;
    RenderPassFlags drawnPasses {0};
    graphics::Material material;
    float radius {0.0f};
    float quadSize {0.0f};
    std::vector<GrassInstance> instances;
};
using ObjectRecord =
    std::variant<RegisteredMesh, RegisteredBillboard, RegisteredParticles, RegisteredGrass>;

struct SceneCounts {
    size_t entries {0};
    size_t rigid {0};
    size_t skinned {0};
    size_t dangly {0};
    size_t saber {0};
    size_t particleEmitters {0};
    size_t particles {0};
    size_t grassNodes {0};
    size_t grassClusters {0};
    size_t billboards {0};
    size_t objects() const { return entries; }
};
std::string formatSceneCounts(const SceneCounts &counts);
std::string renderPassName(RenderPassName pass);

/**
 * GPU-side world-space scene geometry. It owns what is present in this frame
 * and where it is stored; consumers supply their classification and interpret
 * the published buffers according to their own policy.
 */
class GpuScene : boost::noncopyable {
public:
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
    static_assert(offsetof(InstanceMaterial, mainTex) == 80);
    static_assert(offsetof(InstanceMaterial, curatedAlbedoMul) == 112);
    static_assert(offsetof(InstanceMaterial, curatedEmission) == 192);
    static_assert(offsetof(InstanceMaterial, surfaceType) == 208);
    static_assert(offsetof(InstanceMaterial, roughnessScale) == 212);
    static_assert(offsetof(InstanceMaterial, overrideColor) == 224);
    static_assert(offsetof(InstanceMaterial, overrideParams) == 240);
    static_assert(sizeof(InstanceMaterial) == 256);

    /** Three row vectors encode a float3x4 exactly as skin.slang reads it. */
    struct alignas(16) Matrix3x4 {
        glm::vec4 row0 {1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec4 row1 {0.0f, 1.0f, 0.0f, 0.0f};
        glm::vec4 row2 {0.0f, 0.0f, 1.0f, 0.0f};
    };

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
        // Per-quad particle/billboard tint. Meshes and grass write white.
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
        Matrix3x4 transform;
        Matrix3x4 prevTransform;
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
    static_assert(sizeof(Matrix3x4) == 48);
    static_assert(offsetof(SceneObject, srcVertexOffset) == 96);
    static_assert(offsetof(SceneObject, srcIndexOffset) == 100);
    static_assert(offsetof(SceneObject, srcVertexStride) == 104);
    static_assert(offsetof(SceneObject, offPosition) == 108);
    static_assert(offsetof(SceneObject, vertexCount) == 136);
    static_assert(offsetof(SceneObject, materialIndex) == 164);
    static_assert(offsetof(SceneObject, danglyBase) == 168);
    static_assert(offsetof(SceneObject, saberDisplacement) == 176);
    static_assert(sizeof(SceneObject) == 192);

    /** A consumer-defined intersection property, not an acceleration-structure policy. */
    enum class PrimitiveClass { Opaque,
                                NonOpaque };

    /**
     * The requested lifetime of admitted geometry. The implementation may put
     * several objects with the same residency into one region, but it may not
     * infer this classification from a material or a Mesh pointer.
     */
    enum class ResidencyClass { Static,
                                Dynamic };

    /** Trace-specific material lowering is supplied by the current consumer. */
    struct Classification {
        InstanceMaterial material;
        PrimitiveClass primitiveClass {PrimitiveClass::Opaque};
        ResidencyClass residency {ResidencyClass::Dynamic};
        const RegisteredSkin *skin {nullptr};
    };
    using Classifier = std::function<std::optional<Classification>(const RegisteredMesh &)>;
    using GrassClassifier = std::function<std::optional<Classification>(const RegisteredGrass &)>;
    using ParticleClassifier = std::function<std::optional<Classification>(const RegisteredParticles &)>;
    using BillboardClassifier = std::function<std::optional<Classification>(const RegisteredBillboard &)>;

    struct BufferView {
        const graphics::VulkanBuffer *buffer {nullptr};
        VkDeviceSize offset {0};
        VkDeviceSize size {0};
    };
    /**
     * Stable identity of one source triangle within a scene scope. The local
     * primitive is the mesh-face index, not a merged-stream address.
     */
    struct PrimitiveId {
        uint64_t sceneScope {0};
        SceneNodeId object;
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

        /** Resolves a frame-local triangle address without making it an identity. */
        PrimitiveId operator[](uint32_t index) const;
    };

    struct Region {
        ResidencyClass residency {ResidencyClass::Dynamic};
        /**
         * Bumps whenever this region's published geometry or materials change.
         * A retained Static region must bump for an admitted-object transform,
         * admission/removal, material lowering change, or scene-scope change.
         */
        uint64_t revision {0};
        uint32_t firstVertex {0};
        uint32_t vertexCount {0};
        uint32_t firstTriangle {0};
        uint32_t triangleCount {0};
    };
    struct View {
        /**
         * Explicit lifetime scope for PrimitiveId. It changes when GPU scene
         * resources are invalidated, including a module transition; keys from
         * different scopes must never be compared or retained together.
         */
        uint64_t sceneScope {0};
        /**
         * Bumps on every publication in the all-dynamic implementation. This
         * conservatively satisfies every Region invalidation listed above.
         */
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
        /**
         * Frame-local lookup indexed by Region::firstTriangle + local triangle.
         * Primitive indices remain a fast address only; this is the identity a
         * consumer may retain, subject to PrimitiveId::sceneScope and revision.
         */
        PrimitiveIdView primitiveIds;
        /** Frame-local addresses only: admission order may change next frame. */
        std::vector<Region> regions;
    };

    GpuScene();
    ~GpuScene();
    void init(graphics::VulkanRenderer &renderer);
    void deinit();
    void resetFrame();
    void checkIdentityStability();

    void addMesh(RenderCategories categories, SceneNodeId id, SceneNodeNameIds nameIds,
                   graphics::Mesh &mesh, const graphics::Material &material,
                   const glm::mat4 &transform, const glm::mat4 &transformInv,
                   const glm::mat4 &prevTransform, RegisteredDeformation deformation,
                   ModelSceneNode *cullRoot);
    void addBillboard(RenderCategories categories, SceneNodeId id, SceneNodeNameIds nameIds,
                        graphics::Texture &texture, const glm::vec4 &color,
                        const glm::mat4 &transform, const glm::mat4 &transformInv,
                        std::optional<float> size, ModelSceneNode *cullRoot);
    void addParticles(RenderCategories categories, SceneNodeId id, SceneNodeNameIds nameIds,
                        const graphics::Material &material, const glm::ivec2 &gridSize,
                        const std::vector<ParticleInstance> &instances,
                        ModelSceneNode *cullRoot);
    void addGrass(RenderCategories categories, SceneNodeId id, SceneNodeNameIds nameIds,
                    const graphics::Material &material, float radius, float quadSize,
                    const std::vector<GrassInstance> &instances);
    void drawScene(IRenderPassExecutor &executor, RenderFilter filter,
                   VisibilityPolicy visibility);

    bool isObjectEnabled(uint32_t idIndex) const {
        return _disabledObjects.find(idIndex) == _disabledObjects.end();
    }
    void setObjectEnabled(uint32_t idIndex, bool enabled) {
        if (enabled)
            _disabledObjects.erase(idIndex);
        else
            _disabledObjects.insert(idIndex);
    }
    const ModelSceneNode *skyRoom() const { return _skyRoom; }
    void setSkyRoom(const ModelSceneNode *room) { _skyRoom = room; }
    TraceMaterialOverrides &traceMaterials() { return _traceMaterials; }
    const TraceMaterialOverrides &traceMaterials() const { return _traceMaterials; }
    const SceneCounts &counts() const { return _counts; }
    const SceneCounts &drawnCounts() const { return _drawnCounts; }
    const std::map<RenderPassName, SceneCounts> &drawnCountsByPass() const {
        return _drawnCountsByPass;
    }
    const std::vector<ObjectRecord> &objects() const { return _objects; }

    View update(VkCommandBuffer cmd,
                const Classifier &classifier,
                const GrassClassifier &grassClassifier,
                const ParticleClassifier &particleClassifier,
                const BillboardClassifier &billboardClassifier,
                const glm::mat4 &cameraView);

private:
    struct Frame;
    struct SourceGeometry {
        uint32_t vertexOffset {0};
        uint32_t indexOffset {0};
        uint32_t vertexDataCount {0};
        uint32_t indexCount {0};
    };
    std::vector<ObjectRecord> _objects;
    std::unordered_set<uint32_t> _disabledObjects;
    const ModelSceneNode *_skyRoom {nullptr};
    TraceMaterialOverrides _traceMaterials;
    SceneCounts _counts;
    SceneCounts _drawnCounts;
    std::map<RenderPassName, SceneCounts> _drawnCountsByPass;
    std::vector<SceneNodeId> _previousFrameIds;
    size_t _identitySnapshot {0};
    graphics::VulkanRenderer *_renderer {nullptr};
    VkDescriptorSetLayout _mergeLayout {VK_NULL_HANDLE};
    VkDescriptorPool _mergePool {VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> _mergeSets {};
    VkPipelineLayout _mergePipelineLayout {VK_NULL_HANDLE};
    VkPipeline _mergePipeline {VK_NULL_HANDLE};
    std::array<std::unique_ptr<Frame>, 2> _frames;
    std::unordered_map<const graphics::Mesh *, SourceGeometry> _sourceGeometry;
    std::vector<float> _sourceVertexData;
    std::vector<uint32_t> _sourceIndexData;
    std::unique_ptr<graphics::VulkanBuffer> _sourceVertices;
    std::unique_ptr<graphics::VulkanBuffer> _sourceIndices;
    std::vector<std::unique_ptr<graphics::VulkanBuffer>> _retiredSourceBuffers;
    uint32_t _sourceVertexCapacity {0};
    uint32_t _sourceIndexCapacity {0};
    uint64_t _sourceResourceGeneration {0};
    uint64_t _sceneScope {1};
    uint64_t _revision {0};
    bool _inited {false};

    void ensureMergeBuffers(Frame &, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    void clearSourceGeometry();
    const SourceGeometry &appendSourceGeometry(const graphics::Mesh &mesh);
};
} // namespace reone::scene
