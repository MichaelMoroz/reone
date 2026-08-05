/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <glm/glm.hpp>

#include "reone/graphics/frustum.h"
#include "reone/graphics/rendering/gpuscene.h"
#include "reone/graphics/material.h"
#include "reone/scene/node.h"
#include "reone/scene/render/pipeline/tracematerials.h"

namespace reone::graphics {
class Mesh;
class Texture;
} // namespace reone::graphics
namespace reone::scene {
class CameraSceneNode;
class ModelSceneNode;

enum class RenderCategory : uint32_t {
    None = 0,
    Opaque = 1 << 1,
    Transparent = 1 << 2,
    LensFlare = 1 << 3,
};
using RenderCategories = uint32_t;
constexpr RenderCategories renderCategory(RenderCategory category) {
    return static_cast<RenderCategories>(category);
}

/**
 * Grass face budgets bake at this density; the live dial reaches the merge
 * kernel as density/cap in a push constant and gates the active cluster
 * prefix, so a density change costs nothing on the CPU. Must match the
 * editor slider's maximum, or the top of the slider silently clips.
 */
constexpr float kGrassDensityCap = 8.0f;

struct RegisteredSkin {
    const std::vector<glm::mat4> *bones {nullptr};
    const std::vector<glm::mat4> *prevBones {nullptr};
};
struct RegisteredDangly {
    const std::vector<glm::vec4> *positions {nullptr};
    const std::vector<glm::vec4> *prevPositions {nullptr};
};
struct RegisteredSaber {
    const glm::vec3 *displacement {nullptr};
};
using RegisteredDeformation =
    std::variant<std::monostate, RegisteredSkin, RegisteredDangly, RegisteredSaber>;

struct RegisteredMesh {
    RenderCategories categories {0};
    SceneNodeId id;
    SceneNodeNameIds nameIds;
    std::reference_wrapper<graphics::Mesh> mesh;
    graphics::Material material;
    glm::mat4 transform {1.0f};
    glm::mat4 transformInv {1.0f};
    glm::mat4 prevTransform {1.0f};
    RegisteredDeformation deformation;
    ModelSceneNode *cullRoot {nullptr};
};
enum class ProceduralKind {
    Grass,
    Particles,
    Billboard,
};
struct ProceduralInstance {
    int variant {0};
    glm::vec3 position {0.0f};
    glm::vec2 size {0.0f};
    glm::vec4 color {1.0f};
    glm::vec3 right {0.0f};
    glm::vec3 up {0.0f};
    glm::vec2 lightmapUV {0.0f};
    float yaw {0.0f};
};
struct RegisteredProcedural {
    RenderCategories categories {0};
    SceneNodeId id;
    SceneNodeNameIds nameIds;
    graphics::Material material;
    ProceduralKind kind {ProceduralKind::Grass};
    glm::ivec2 gridSize {1};
    std::vector<ProceduralInstance> instances;
    std::vector<graphics::ProceduralQuad> loweredQuads;
    const std::vector<graphics::GrassFace> *grassFaces {nullptr};
    size_t grassClusterCount {0};
    uint64_t grassGeneration {0};
    ModelSceneNode *cullRoot {nullptr};
    size_t instanceCount() const {
        if (kind == ProceduralKind::Grass) {
            return grassClusterCount;
        }
        return loweredQuads.empty() ? instances.size() : loweredQuads.size();
    }
};
using ObjectRecord = std::variant<RegisteredMesh, RegisteredProcedural>;

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

/**
 * GPU-side world-space scene geometry. It owns what is present in this frame
 * and where it is stored; shared admission supplies classification and lowers
 * the published buffers for both raster and tracing consumers.
 */
class GpuScene : boost::noncopyable {
public:
    using InstanceMaterial = graphics::InstanceMaterial;
    using Matrix3x4 = graphics::Matrix3x4;
    using MergedVertex = graphics::MergedVertex;
    using SceneObject = graphics::SceneObject;
    using PrimitiveClass = graphics::GpuScenePrimitiveClass;
    using ResidencyClass = graphics::GpuSceneResidencyClass;

    enum class AdmissionKind {
        Opaque,
        Cutout,
        LitBlended,
        AdditiveEmissive,
    };

    /** Material lowering and the authoritative coverage kind for one object. */
    struct Classification {
        InstanceMaterial material;
        AdmissionKind kind {AdmissionKind::Opaque};
        ResidencyClass residency {ResidencyClass::Dynamic};
        const RegisteredSkin *skin {nullptr};
    };
    using Classifier = std::function<std::optional<Classification>(const RegisteredMesh &)>;
    using ProceduralClassifier =
        std::function<std::optional<Classification>(const RegisteredProcedural &)>;

    /** Drop only streams whose contents are defined afresh every frame. */
    void resetFrame();
    void beginFullCollection();
    void endFullCollection();
    /** Drop the persistent world and all admission/intern state. */
    void clear();
    void resetAdmissionCache();
    void checkIdentityStability();

    void unregisterObject(SceneNodeId id);
    void setObjectActive(SceneNodeId id, bool active);
    void updateMeshTransform(SceneNodeId id, const glm::mat4 &transform,
                             const glm::mat4 &transformInv,
                             const glm::mat4 &prevTransform);
    void settleMeshTransform(SceneNodeId id, const glm::mat4 &transform);
    bool updateMeshDeformation(SceneNodeId id, RegisteredDeformation deformation);
    void patchBumpMapFrame(SceneNodeId id, int frame);
    void patchMaterialUv(SceneNodeId id, const glm::mat3x4 &uv);
    void dirtyAdmission() { ++_admissionGeneration; }
    uint64_t admissionGeneration() const { return _admissionGeneration; }
    void setShadowScene(GpuScene *scene) { _shadowScene = scene; }
    GpuScene *shadowScene() const { return _shadowScene; }

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
                      std::vector<graphics::ProceduralQuad> quads,
                      ModelSceneNode *cullRoot);
    void addGrass(RenderCategories categories, SceneNodeId id, SceneNodeNameIds nameIds,
                  const graphics::Material &material,
                  const std::vector<graphics::GrassFace> &faces,
                  uint64_t grassGeneration);
    bool isObjectEnabled(uint32_t idIndex) const {
        return _disabledObjects.find(idIndex) == _disabledObjects.end();
    }
    bool isObjectActive(SceneNodeId id) const { return isActive(id); }
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
    const std::vector<ObjectRecord> &objects() const { return _objects; }

    graphics::GpuSceneUpload prepare(const Classifier &classifier,
                                     const ProceduralClassifier &proceduralClassifier,
                                     const glm::mat4 &cameraView,
                                     uint64_t admissionGeneration,
                                     bool forceFull = false,
                                     graphics::GpuSceneUpload reuse = {});

private:
    struct CachedClassification {
        bool dirty {true};
        bool classified {false};
        std::optional<Classification> value;
        uint32_t materialIndex {0};
        uint64_t admissionGeneration {0};
    };
    struct InternedMaterial {
        InstanceMaterial material;
        uint32_t references {0};
        uint64_t hash {0};
        bool occupied {false};
    };

    std::vector<ObjectRecord> _objects;
    std::unordered_map<SceneNodeId, CachedClassification> _classification;
    std::vector<InternedMaterial> _materials;
    std::unordered_map<uint64_t, std::vector<uint32_t>> _materialIndices;
    std::vector<uint32_t> _freeMaterialIndices;
    std::unordered_set<SceneNodeId> _inactiveObjects;
    std::unordered_set<SceneNodeId> _fullCollectionUnseen;
    std::unordered_set<uint32_t> _disabledObjects;
    const ModelSceneNode *_skyRoom {nullptr};
    TraceMaterialOverrides _traceMaterials;
    SceneCounts _counts;
    std::vector<SceneNodeId> _previousFrameIds;
    size_t _identitySnapshot {0};
    uint64_t _admissionGeneration {1};
    uint64_t _grassFaceGeneration {1};
    GpuScene *_shadowScene {nullptr};

    static SceneNodeId objectId(const ObjectRecord &object);
    static bool idLess(SceneNodeId left, SceneNodeId right);
    std::vector<ObjectRecord>::iterator findObject(SceneNodeId id);
    std::vector<ObjectRecord>::const_iterator findObject(SceneNodeId id) const;
    void upsert(ObjectRecord object);
    void eraseObject(std::vector<ObjectRecord>::iterator it);
    void addCounts(const ObjectRecord &object);
    void removeCounts(const ObjectRecord &object);
    uint32_t internMaterial(const InstanceMaterial &material);
    void releaseMaterial(uint32_t index);
    void invalidate(SceneNodeId id);
    void dirtyGrassFaces();
    bool isActive(SceneNodeId id) const;
};
} // namespace reone::scene
