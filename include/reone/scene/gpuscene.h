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
#include "reone/graphics/gpuscene.h"
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
    ShadowCaster = 1 << 0,
    Opaque = 1 << 1,
    Transparent = 1 << 2,
    LensFlare = 1 << 3,
};
using RenderCategories = uint32_t;
constexpr RenderCategories renderCategory(RenderCategory category) {
    return static_cast<RenderCategories>(category);
}

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
    float quadSize {0.0f};
    std::vector<ProceduralInstance> instances;
    ModelSceneNode *cullRoot {nullptr};
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
    using InstanceMaterial = graphics::GpuSceneMaterial;
    using Matrix3x4 = graphics::GpuSceneMatrix3x4;
    using MergedVertex = graphics::GpuSceneMergedVertex;
    using SceneObject = graphics::GpuSceneObjectData;
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
    const std::vector<ObjectRecord> &objects() const { return _objects; }

    graphics::GpuSceneUpload prepare(const Classifier &classifier,
                                     const ProceduralClassifier &proceduralClassifier,
                                     const glm::mat4 &cameraView) const;

private:
    std::vector<ObjectRecord> _objects;
    std::unordered_set<uint32_t> _disabledObjects;
    const ModelSceneNode *_skyRoom {nullptr};
    TraceMaterialOverrides _traceMaterials;
    SceneCounts _counts;
    std::vector<SceneNodeId> _previousFrameIds;
    size_t _identitySnapshot {0};
};
} // namespace reone::scene
