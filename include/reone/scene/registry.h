/*
 * Copyright (c) 2020-2026 The reone project contributors
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

#pragma once

#include <variant>
#include <unordered_set>

#include "reone/graphics/frustum.h"
#include "reone/graphics/material.h"
#include "reone/scene/node.h"

namespace reone {

namespace graphics {

class Mesh;
class Texture;

} // namespace graphics

namespace scene {

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
    Debug
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
    Debug = 1 << 4
};

using RenderCategories = uint32_t;

constexpr RenderCategories renderCategory(RenderCategory category) {
    return static_cast<RenderCategories>(category);
}

struct RenderFilter {
    RenderPassName pass {RenderPassName::None};
    RenderCategory category {RenderCategory::None};
};

enum class VisibilityPolicyKind {
    ViewCamera,
    Frusta,
    None
};

/**
 * Per-pass visibility selection for the frame snapshot.
 *
 * The draw-distance limit is orthogonal to which volume is tested: it applies
 * whenever a camera is supplied, so a shadow pass can cull against the light
 * while still dropping what is too far away to matter. Measured at frame 900
 * of danm14ab, the light frusta admit two casters the camera frustum rejected;
 * dropping draw distance as well admits a hundred, none of which changed a
 * pixel. Correctness comes from the frusta, so the limit stays.
 */
struct VisibilityPolicy {
    VisibilityPolicyKind kind {VisibilityPolicyKind::ViewCamera};
    const CameraSceneNode *drawDistanceCamera {nullptr};
    const graphics::Frustum *lightFrusta {nullptr};
    size_t numLightFrusta {0};

    static VisibilityPolicy viewCamera(const CameraSceneNode *camera) {
        return {VisibilityPolicyKind::ViewCamera, camera, nullptr, 0};
    }

    static VisibilityPolicy shadowFrusta(const graphics::Frustum *frusta,
                                         size_t numFrusta,
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
};

struct RegisteredSkin {
    std::vector<glm::mat4> bones;
    std::vector<glm::mat4> prevBones;
};

struct RegisteredDangly {
    std::vector<glm::vec4> positions;
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

struct RegisteredAABB {
    RenderCategories categories {0};
    SceneNodeId id;
    SceneNodeNameIds nameIds;
    RenderPassFlags drawnPasses {0};
    std::vector<glm::vec4> corners;
    ModelSceneNode *cullRoot {nullptr};
};

struct RegisteredDebug {
    RenderCategories categories {renderCategory(RenderCategory::Debug)};
    std::function<void()> execute;
};

using RegisteredObject =
    std::variant<RegisteredMesh, RegisteredBillboard, RegisteredParticles, RegisteredGrass,
                 RegisteredAABB, RegisteredDebug>;

struct RegistryCounts {
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
    size_t aabbs {0};

    size_t objects() const {
        return entries;
    }
};

std::string formatRegistryCounts(const RegistryCounts &counts);
std::string renderPassName(RenderPassName pass);

/**
 * An ordered snapshot of everything submitted by the frame's registration.
 *
 * Animated state and materials are copied because their callers build them on
 * the stack. Category flags describe the object's nature independently of the
 * pass that later selects it.
 */
class RenderRegistry {
public:
    void resetFrame();

    /** Per-object debug kill switch, driven from the registry panel. Keyed
        by SceneNodeId index so it survives the per-frame re-registration;
        enforced by drawScene and the TLAS admission alike, so a disabled
        object vanishes from every render mode. */
    /** The winning sky room of the last traced frame, published by the
        ray-query admission for the registry panel. Null when not tracing. */
    const ModelSceneNode *skyRoom() const { return _skyRoom; }
    void setSkyRoom(const ModelSceneNode *room) { _skyRoom = room; }

    bool isObjectEnabled(uint32_t idIndex) const {
        return _disabledObjects.find(idIndex) == _disabledObjects.end();
    }
    void setObjectEnabled(uint32_t idIndex, bool enabled) {
        if (enabled) {
            _disabledObjects.erase(idIndex);
        } else {
            _disabledObjects.insert(idIndex);
        }
    }
    void beginSceneTraversal();
    void checkIdentityStability();

    void registerMesh(RenderCategories categories,
                 SceneNodeId id,
                 SceneNodeNameIds nameIds,
                 graphics::Mesh &mesh,
                 const graphics::Material &material,
                 const glm::mat4 &transform,
                 const glm::mat4 &transformInv,
                 const glm::mat4 &prevTransform,
                 RegisteredDeformation deformation,
                 ModelSceneNode *cullRoot);

    void registerBillboard(RenderCategories categories,
                      SceneNodeId id,
                      SceneNodeNameIds nameIds,
                      graphics::Texture &texture,
                      const glm::vec4 &color,
                      const glm::mat4 &transform,
                      const glm::mat4 &transformInv,
                      std::optional<float> size,
                      ModelSceneNode *cullRoot);

    void registerParticles(RenderCategories categories,
                      SceneNodeId id,
                      SceneNodeNameIds nameIds,
                      const graphics::Material &material,
                      const glm::ivec2 &gridSize,
                      const std::vector<ParticleInstance> &instances,
                      ModelSceneNode *cullRoot);

    void registerGrass(RenderCategories categories,
                  SceneNodeId id,
                  SceneNodeNameIds nameIds,
                  const graphics::Material &material,
                  float radius,
                  float quadSize,
                  const std::vector<GrassInstance> &instances);

    void registerAABB(RenderCategories categories,
                 SceneNodeId id,
                 SceneNodeNameIds nameIds,
                 const std::vector<glm::vec4> &corners,
                 ModelSceneNode *cullRoot);
    void addDebug(std::function<void()> execute);

    void drawScene(IRenderPassExecutor &executor,
                   RenderFilter filter,
                   VisibilityPolicy visibility);

    const RegistryCounts &registeredCounts() const { return _registeredCounts; }
    const RegistryCounts &drawnCounts() const { return _drawnCounts; }
    const std::map<RenderPassName, RegistryCounts> &drawnCountsByPass() const {
        return _drawnCountsByPass;
    }
    const std::vector<RegisteredObject> &objects() const { return _objects; }
    size_t traversalCount() const { return _traversalCount; }

private:
    std::vector<RegisteredObject> _objects;
    std::unordered_set<uint32_t> _disabledObjects;
    const ModelSceneNode *_skyRoom {nullptr};
    size_t _traversalCount {0};
    RegistryCounts _registeredCounts;
    RegistryCounts _drawnCounts;
    std::map<RenderPassName, RegistryCounts> _drawnCountsByPass;

    // Retained only while Graphics logging is enabled, for capture diagnostics.
    std::vector<SceneNodeId> _previousFrameIds;
    size_t _identitySnapshot {0};
};

} // namespace scene

} // namespace reone
