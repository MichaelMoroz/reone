/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include <cstdint>
#include <functional>
#include <limits>

#include "reone/graphics/aabb.h"

#include "types.h"

namespace reone {

namespace graphics {

struct GraphicsServices;

} // namespace graphics

namespace audio {

struct AudioServices;

}

namespace resource {

struct ResourceServices;

}

namespace scene {

class GpuScene;
class ISceneGraph;
class IUser;
class SceneGraph;

/**
 * Stable identity of a scene node.
 *
 * The default value is invalid. The generation remains zero while SceneGraph
 * never destroys nodes; it becomes meaningful once node slots can be reused.
 */
struct SceneNodeId {
    static constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();

    uint32_t index {kInvalid};
    uint32_t generation {kInvalid};

    constexpr bool operator==(const SceneNodeId &other) const {
        return index == other.index && generation == other.generation;
    }
    constexpr bool isValid() const { return index != kInvalid; }
};

/**
 * Interned labels that make a scene-node identity readable in diagnostics.
 * Zero deliberately means that this node has no model or model-node label.
 */
struct SceneNodeNameIds {
    uint32_t model {0};
    uint32_t node {0};
};

class SceneNode : boost::noncopyable {
public:
    void addChild(SceneNode &node);
    void removeChild(SceneNode &node);
    void removeAllChildren();

    virtual void update(float dt);

    virtual void collectLeafs(GpuScene &scene, const std::vector<SceneNode *> &leafs) {
    }

    bool isEnabled() const { return _enabled; }
    bool isCulled() const { return _culled; }
    bool isCullingEnabled() const { return _cullingEnabled; }
    bool isPoint() const { return _point; }

    glm::vec3 origin() const;
    glm::vec2 origin2D() const;

    float getDistanceTo(const glm::vec3 &point) const;
    float getDistanceTo(const SceneNode &other) const;
    float getSquareDistanceTo(const glm::vec3 &point) const;
    float getSquareDistanceTo(const SceneNode &other) const;
    float getSquareDistanceTo2D(const glm::vec2 &point) const;

    glm::vec3 getWorldCenterOfAABB() const;

    SceneNodeType type() const { return _type; }
    SceneNodeId id() const { return _id; }
    SceneNodeNameIds nameIds() const { return _nameIds; }
    void setNameIds(SceneNodeNameIds ids) { _nameIds = ids; }
    SceneNode *parent() { return _parent; }
    const SceneNode *parent() const { return _parent; }
    const std::vector<SceneNode *> &children() const { return _children; }
    const graphics::AABB &aabb() const { return _aabb; }
    IUser *user() { return _user; }
    const IUser *user() const { return _user; }
    ISceneGraph &graph() { return _sceneGraph; }

    void setUser(IUser &user) {
        _user = &user;
    }

    // Flags

    void setEnabled(bool enabled);
    void setGpuSubtreeActive(bool active);
    void setCulled(bool culled) { _culled = culled; }
    void setCullingEnabled(bool enabled) { _cullingEnabled = enabled; }

    // END Flags

    // Transformations

    const glm::mat4 &localTransform() const { return _localTransform; }
    const glm::mat4 &absoluteTransform() const { return _absTransform; }
    const glm::mat4 &absoluteTransformInverse() const { return _absTransformInv; }

    /**
     * Absolute transform as of the end of the previous rendered frame. Used to
     * produce motion vectors. Equals the current transform until this node has
     * been rendered at least once.
     */
    const glm::mat4 &previousAbsoluteTransform() const { return _prevAbsTransform; }

    /**
     * Latch the current transform as the previous one, at most once per frame.
     * Called at the end of SceneGraph::render, so that a node drawn by several
     * passes within one frame reports the same previous transform to each.
     */
    virtual void snapshotPreviousFrame(uint64_t frame) {
        if (_prevFrame == frame) {
            return;
        }
        _prevAbsTransform = _absTransform;
        _prevFrame = frame;
    }

    void setLocalTransform(glm::mat4 transform);

    // END Transformations

protected:
    SceneNodeType _type;
    ISceneGraph &_sceneGraph;
    graphics::GraphicsServices &_graphicsSvc;
    audio::AudioServices &_audioSvc;
    resource::ResourceServices &_resourceSvc;

    SceneNodeId _id;
    SceneNodeNameIds _nameIds;

    SceneNode *_parent {nullptr};
    /**
     * Insertion-ordered, deliberately.
     *
     * This was an unordered_set keyed on the pointer. Pointer values differ
     * between processes, so its iteration order did too, and that order decides
     * the order children are updated and drawn in. Emitters draw from the one
     * shared random generator as they spawn particles, so a reordering handed
     * each emitter a different part of the sequence, and transparent geometry
     * is composited in traversal order. Two runs of the same build rendered
     * differently, which made A/B comparison between backends unreliable.
     */
    std::vector<SceneNode *> _children;

    graphics::AABB _aabb;

    IUser *_user {nullptr};

    // Flags

    bool _enabled {true};
    bool _culled {false};
    bool _point {true}; /**< is this node represented by a single point?  */

    /**
     * Can this node be culled?
     *
     * For some nodes we disable culling because their position is changed
     * significantly by animations. For example, a node may be at zero position,
     * which is out of frame, but an animation moves in frame.
     */
    bool _cullingEnabled {true};

    // END Flags

    // Transformations

    glm::mat4 _localTransform {1.0f};
    glm::mat4 _absTransform {1.0f};
    glm::mat4 _absTransformInv {1.0f};
    glm::mat4 _prevAbsTransform {1.0f};
    uint64_t _prevFrame {0}; /**< frame the previous transform was latched on */

    // END Transformations

    SceneNode(
        SceneNodeType type,
        ISceneGraph &sceneGraph,
        graphics::GraphicsServices &graphicsSvc,
        audio::AudioServices &audioSvc,
        resource::ResourceServices &resourceSvc) :
        _type(type),
        _sceneGraph(sceneGraph),
        _graphicsSvc(graphicsSvc),
        _audioSvc(audioSvc),
        _resourceSvc(resourceSvc) {
    }

    void computeAbsoluteTransforms();

    virtual void onAbsoluteTransformChanged() {}
    virtual void onGpuActivationChanged(bool active) {}

private:
    friend class SceneGraph;

    void setId(SceneNodeId id) { _id = id; }
    void refreshGpuActivation(bool ancestorsActive);
};

} // namespace scene

} // namespace reone

template <>
struct std::hash<reone::scene::SceneNodeId> {
    size_t operator()(const reone::scene::SceneNodeId &id) const noexcept {
        auto value = (static_cast<uint64_t>(id.generation) << 32) | id.index;
        return std::hash<uint64_t> {}(value);
    }
};
