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

#include "reone/scene/graph.h"

#include "reone/system/profiler.h"

#include "reone/audio/di/services.h"
#include "reone/graphics/camera/perspective.h"
#include "reone/graphics/di/services.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/options.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/walkmesh.h"
#include "reone/scene/collision.h"
#include "reone/scene/node/camera.h"
#include "reone/scene/node/emitter.h"
#include "reone/scene/node/grass.h"
#include "reone/scene/node/light.h"
#include "reone/scene/node/mesh.h"
#include "reone/scene/node/model.h"
#include "reone/scene/node/particle.h"
#include "reone/scene/node/sound.h"
#include "reone/scene/node/trigger.h"
#include "reone/scene/node/walkmesh.h"
#include "reone/scene/render/pipeline.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

uint32_t SceneGraph::internName(std::string_view name) {
    if (name.empty()) {
        return 0;
    }
    auto [it, inserted] = _nameIds.try_emplace(std::string(name), static_cast<uint32_t>(_names.size()));
    if (inserted) {
        _names.push_back(it->first);
    }
    return it->second;
}

std::string_view SceneGraph::nameText(uint32_t id) const {
    return id < _names.size() ? _names[id] : std::string_view {};
}

static constexpr int kMaxFlareLights = 4;
static constexpr int kMaxSoundCount = 4;

static constexpr float kShadowFadeSpeed = 2.0f;

static constexpr float kLightRadiusBias = 64.0f;

static constexpr float kMaxCollisionDistanceWalk = 8.0f;
static constexpr float kMaxCollisionDistanceWalk2 = kMaxCollisionDistanceWalk * kMaxCollisionDistanceWalk;

static constexpr float kMaxCollisionDistanceLineOfSight = 16.0f;
static constexpr float kMaxCollisionDistanceLineOfSight2 = kMaxCollisionDistanceLineOfSight * kMaxCollisionDistanceLineOfSight;

static constexpr float kPointLightShadowsFOV = glm::radians(90.0f);
static constexpr float kPointLightShadowsNearPlane = 0.25f;
static constexpr float kPointLightShadowsFarPlane = 2500.0f;

static const std::vector<float> g_shadowCascadeDivisors {
    0.005f,
    0.015f,
    0.045f,
    0.135f};

glm::vec3 SceneGraph::shadowLightDirection() const {
    auto authored = _shadowLight->direction();
    if (_shadowLight->hasAuthoredDirection()) {
        return authored;
    }

    // Identity is the model format's default orientation. Aim such lights at
    // the centre of the module's room geometry: unlike the camera or world
    // origin, these bounds are fixed for the lifetime of the loaded area.
    AABB bounds;
    for (auto &root : _modelRoots) {
        if (root->usage() != ModelUsage::Room || root->isBackgroundScenery()) {
            continue;
        }
        bounds.expand(root->aabb() * root->absoluteTransform());
    }
    if (!bounds.isDegenerate()) {
        auto centre = 0.5f * (bounds.min() + bounds.max());
        auto direction = centre - shadowLightPosition();
        if (glm::length2(direction) >= glm::epsilon<float>()) {
            return glm::normalize(direction);
        }
    }
    return authored;
}

void SceneGraph::clear() {
    _modelRoots.clear();
    _walkmeshRoots.clear();
    _groundHeightDirty = true;
    _triggerRoots.clear();
    _soundRoots.clear();
    _grassRoots.clear();
    _meshes.clear();
    _lights.clear();
    _emitters.clear();
    _opaqueLeafs.clear();
    _transparentLeafs.clear();
    _flareLights.clear();
    _activeLights.clear();
    _registeredFlareLights.clear();
    _gpuScene.clear();
    _shadowGpuScene.clear();
    _incrementalSceneReady = false;
    _shadowProperties = {};
    // The shadow light is a bare pointer into the lights just destroyed above.
    // Leaving it set is a dangling read, and it also strands the sun: the next
    // update sees a light already chosen, so it fades the old one out before it
    // will look for a new one, and then adopts that one dimmed because the
    // no-cross-fade-on-a-fresh-scene path is gated on there having been none.
    // The module gets no sun until something else resets this.
    _shadowLight = nullptr;
    _shadowActive = false;
    _shadowStrength = 0.0f;
    // The pipeline outlives the scene it drew, and everything temporal it holds
    // describes geometry that no longer exists: NRD's accumulation, the common
    // tail's resolve history, the previous view and projection the reprojection
    // is built from. Carried into a new module they are not stale so much as
    // wrong - the reprojection maps them onto whatever now occupies those
    // pixels, and the filters spend their convergence dragging one area's light
    // off another's surfaces.
    //
    // Here rather than in Game::loadModule because this is the point every
    // discontinuity already passes through - a warp, a transition, a save load -
    // and a caller that empties the scene should not also have to remember
    // this.
    // A rebuild, not just a history restart. The pipeline caches more of the
    // module than its temporal filters: the sky bake, the admission layer and
    // the device-side scene all outlive a clear, so the next module renders
    // against the previous one's sky and its lighting is wrong until a graphics
    // Apply happens to throw the same things away. This is what makes a warp or
    // a save load reach as far as Apply does.
    //
    // Requested rather than performed - see consumeRenderPipelineRebuild.
    if (_renderPipeline) {
        _renderPipeline->restartTemporalHistory();
        _renderPipelineRebuildRequested = true;
    }
}

void SceneGraph::addRoot(std::shared_ptr<ModelSceneNode> node) {
    node->setGpuSubtreeActive(true);
    _modelRoots.push_back(std::move(node));
}

void SceneGraph::addRoot(std::shared_ptr<WalkmeshSceneNode> node) {
    _walkmeshRoots.push_back(std::move(node));
    _groundHeightDirty = true;
}

std::optional<float> SceneGraph::groundHeight() const {
    if (!_groundHeightDirty) {
        return _groundHeight;
    }
    _groundHeightDirty = false;
    _groundHeight.reset();
    double weighted = 0.0;
    double area = 0.0;
    for (const auto &root : _walkmeshRoots) {
        const auto transform = root->absoluteTransform();
        for (const auto &face : root->walkmesh().faces()) {
            if (face.vertices.size() < 3) {
                continue;
            }
            const auto a = glm::vec3(transform * glm::vec4(face.vertices[0], 1.0f));
            const auto b = glm::vec3(transform * glm::vec4(face.vertices[1], 1.0f));
            const auto c = glm::vec3(transform * glm::vec4(face.vertices[2], 1.0f));
            const float faceArea = 0.5f * glm::length(glm::cross(b - a, c - a));
            if (!(faceArea > 0.0f)) {
                continue;
            }
            weighted += static_cast<double>(faceArea) * (a.z + b.z + c.z) / 3.0;
            area += faceArea;
        }
    }
    if (area > 0.0) {
        _groundHeight = static_cast<float>(weighted / area);
    }
    return _groundHeight;
}

void SceneGraph::addRoot(std::shared_ptr<TriggerSceneNode> node) {
    _triggerRoots.push_back(std::move(node));
}

void SceneGraph::addRoot(std::shared_ptr<GrassSceneNode> node) {
    node->setGpuSubtreeActive(true);
    _grassRoots.push_back(std::move(node));
}

void SceneGraph::addRoot(std::shared_ptr<SoundSceneNode> node) {
    _soundRoots.push_back(std::move(node));
}

void SceneGraph::removeRoot(ModelSceneNode &node) {
    node.setGpuSubtreeActive(false);
    for (auto it = _activeLights.begin(); it != _activeLights.end();) {
        if (&(*it)->model() == &node) {
            it = _activeLights.erase(it);
        } else {
            ++it;
        }
    }
    auto it = std::remove_if(
        _modelRoots.begin(),
        _modelRoots.end(),
        [&node](auto &root) { return root.get() == &node; });
    _modelRoots.erase(it, _modelRoots.end());
}

void SceneGraph::removeRoot(WalkmeshSceneNode &node) {
    auto it = std::remove_if(
        _walkmeshRoots.begin(),
        _walkmeshRoots.end(),
        [&node](auto &root) { return root.get() == &node; });
    _walkmeshRoots.erase(it, _walkmeshRoots.end());
    _groundHeightDirty = true;
}

void SceneGraph::removeRoot(TriggerSceneNode &node) {
    auto it = std::remove_if(
        _triggerRoots.begin(),
        _triggerRoots.end(),
        [&node](auto &root) { return root.get() == &node; });
    _triggerRoots.erase(it, _triggerRoots.end());
}

void SceneGraph::removeRoot(GrassSceneNode &node) {
    node.setGpuSubtreeActive(false);
    auto it = std::remove_if(
        _grassRoots.begin(),
        _grassRoots.end(),
        [&node](auto &root) { return root.get() == &node; });
    _grassRoots.erase(it, _grassRoots.end());
}

void SceneGraph::removeRoot(SoundSceneNode &node) {
    auto it = std::remove_if(
        _soundRoots.begin(),
        _soundRoots.end(),
        [&node](auto &root) { return root.get() == &node; });
    _soundRoots.erase(it, _soundRoots.end());
}

void SceneGraph::update(float dt) {
    R_PROFILE_ZONE("SceneGraph::update");
    // Advanced here rather than read from a wall clock, so it stops when the
    // scene stops: the freeze-frame diagnostic holds dt at zero to prove the
    // filters converge on a scene that is not moving, and a clock that kept
    // running would keep the grass moving under it.
    _prevTime = _time;
    _time += dt;
    if (_updateRoots) {
        for (auto &root : _modelRoots) {
            root->update(dt);
        }
        for (auto &root : _grassRoots) {
            root->update(dt);
        }
        for (auto &root : _soundRoots) {
            root->update(dt);
        }
    }
    if (!_activeCamera) {
        return;
    }
    refresh();
    updateLighting();
    updateShadowLight(dt);
    updateFlareLights();
    {
        R_PROFILE_ZONE("SceneGraph::flare visibility update");
        std::unordered_set<LightSceneNode *> visible;
        for (auto *light : _flareLights) {
            Collision collision;
            // The dial belongs here, not only on the collection path: this is
            // where a flare is registered every frame, so leaving it ungated
            // meant the switch controlled nothing.
            if (!_graphicsOpt.lensFlares ||
                testLineOfSight(_activeCamera->origin(), light->origin(), collision)) {
                _gpuScene.unregisterObject(light->id());
                continue;
            }
            light->collectLensFlare(
                _gpuScene, light->modelNode().light()->flares.front());
            visible.insert(light);
        }
        if (visible.size() != _loggedFlareVisible) {
            _loggedFlareVisible = visible.size();
            const glm::vec3 eye = _activeCamera ? _activeCamera->origin() : glm::vec3 {0.0f};
            debug("Scene '" + _name + "': " + std::to_string(visible.size()) +
                      " flares registered this frame, camera at (" +
                      std::to_string(eye.x) + ", " + std::to_string(eye.y) + ", " +
                      std::to_string(eye.z) + ")",
                  LogChannel::Graphics);
        }
        for (auto *light : _registeredFlareLights) {
            if (visible.find(light) == visible.end())
                _gpuScene.unregisterObject(light->id());
        }
        _registeredFlareLights = std::move(visible);
    }
    updateSounds();
    prepareOpaqueLeafs();
    prepareTransparentLeafs();
    {
        R_PROFILE_ZONE("SceneGraph::particle stream update");
        std::unordered_set<EmitterSceneNode *> collected;
        for (auto &[node, leafs] : _transparentLeafs) {
            if (node->type() != SceneNodeType::Emitter)
                continue;
            auto *emitter = static_cast<EmitterSceneNode *>(node);
            emitter->collectLeafs(_gpuScene, leafs);
            collected.insert(emitter);
        }
        for (auto *emitter : _emitters) {
            if (collected.find(emitter) == collected.end())
                _gpuScene.unregisterObject(emitter->id());
        }
    }
}

void SceneGraph::updateLighting() {
    R_PROFILE_ZONE("SceneGraph::updateLighting");
    // Find closest lights and create a lookup. The option, not the array
    // ceiling: the block is sized for the worst case once, and this is how many
    // of its slots a frame is allowed to fill.
    const int lightBudget = std::clamp(_graphicsOpt.maxLights, 1, kMaxLights);
    auto closestLights = computeClosestLights(lightBudget, [](auto &light, float distance2) {
        float radius = light.radius() + kLightRadiusBias;
        return distance2 < radius * radius;
    });
    std::set<LightSceneNode *> lookup;
    for (auto &light : closestLights) {
        lookup.insert(light);
    }
    // De-activate active lights, unless found in a lookup. Active lights are removed from the lookup
    for (auto &light : _activeLights) {
        if (lookup.count(light) == 0) {
            light->setActive(false);
        } else {
            lookup.erase(light);
        }
    }
    // Remove active lights that are inactive and completely faded
    for (auto it = _activeLights.begin(); it != _activeLights.end();) {
        auto light = *it;
        if ((!light->isActive() && light->strength() == 0.0f) || (!light->model().isEnabled())) {
            it = _activeLights.erase(it);
        } else {
            ++it;
        }
    }
    // Add closest lights to active lights
    for (auto &light : lookup) {
        if (_activeLights.size() >= kMaxLights) {
            return;
        }
        light->setActive(true);
        _activeLights.push_back(light);
    }
}

void SceneGraph::updateShadowLight(float dt) {
    const bool hadShadowLight = _shadowLight != nullptr;
    auto closestLights = computeClosestLights(1, [this](auto &light, float distance2) {
        if (!light.modelNode().light()->shadow) {
            return false;
        }
        float radius = light.radius();
        return distance2 < radius * radius;
    });
    if (_shadowLight) {
        if (closestLights.empty() || _shadowLight != closestLights.front()) {
            _shadowActive = false;
        }
        if (_shadowActive) {
            _shadowStrength = glm::min(1.0f, _shadowStrength + kShadowFadeSpeed * dt);
        } else {
            _shadowStrength = glm::max(0.0f, _shadowStrength - kShadowFadeSpeed * dt);
            if (_shadowStrength == 0.0f) {
                _shadowLight = nullptr;
            }
        }
    }
    if (!_shadowLight && !closestLights.empty()) {
        _shadowLight = closestLights.front();
        _shadowActive = true;
        // There is no previous shadow to cross-fade on the first light in a
        // freshly loaded scene. Starting it dim only makes the module visibly
        // brighten during its opening frames.
        if (!hadShadowLight) {
            _shadowStrength = 1.0f;
        }
        auto direction = shadowLightDirection();
        auto position = shadowLightPosition();
        auto orientation = _shadowLight->modelNode().restOrientation();
        std::ostringstream ss;
        ss << "Scene '" << _name << "': shadow light '" << _shadowLight->modelNode().name()
           << "' aim=" << (_shadowLight->hasAuthoredDirection() ? "authored" : "room-bounds")
           << " position=(" << position.x << ", " << position.y << ", " << position.z
           << ") orientation=(" << orientation.w << ", " << orientation.x << ", "
           << orientation.y << ", " << orientation.z << ") direction=(" << direction.x
           << ", " << direction.y << ", " << direction.z << ")";
        info(ss.str(), LogChannel::Graphics);
    }
    if (hadShadowLight != (_shadowLight != nullptr))
        _incrementalSceneReady = false;
}

void SceneGraph::updateFlareLights() {
    size_t authored = 0;
    for (const auto &light : _lights) {
        if (!light->modelNode().light()->flares.empty()) ++authored;
    }
    _flareLights = computeClosestLights(kMaxFlareLights, [](auto &light, float distance2) {
        if (light.modelNode().light()->flares.empty()) {
            return false;
        }
        float radius = light.modelNode().light()->flareRadius;
        return distance2 < radius * radius;
    });
    // Says whether a scene has flares to draw at all, separately from whether
    // any is close enough this frame. Without the first number an empty frame
    // and an unreachable draw path look identical.
    if (authored != _loggedFlareLights) {
        _loggedFlareLights = authored;
        std::string where;
        if (!_flareLights.empty()) {
            const glm::vec3 origin = _flareLights.front()->origin();
            where = "; nearest at (" + std::to_string(origin.x) + ", " +
                    std::to_string(origin.y) + ", " + std::to_string(origin.z) + ")";
        }
        debug("Scene '" + _name + "': " + std::to_string(authored) +
                  " lights author flares, " + std::to_string(_flareLights.size()) +
                  " in range" + where,
              LogChannel::Graphics);
    }
}

void SceneGraph::updateSounds() {
    std::vector<std::pair<SoundSceneNode *, float>> distances;
    glm::vec3 cameraPos(_activeCamera->localTransform()[3]);

    // For each sound, calculate its distance to the camera
    for (auto &root : _soundRoots) {
        root->setAudible(false);
        if (!root->isEnabled()) {
            continue;
        }
        float dist2 = root->getSquareDistanceTo(cameraPos);
        float maxDist2 = root->maxDistance() * root->maxDistance();
        if (dist2 > maxDist2) {
            continue;
        }
        distances.push_back(std::make_pair(root.get(), dist2));
    }

    // Take up to N most closest sounds to the camera
    sort(distances.begin(), distances.end(), [](auto &left, auto &right) {
        int leftPriority = left.first->priority();
        int rightPriority = right.first->priority();
        if (leftPriority < rightPriority) {
            return true;
        }
        if (leftPriority > rightPriority) {
            return false;
        }
        return left.second < right.second;
    });
    if (distances.size() > kMaxSoundCount) {
        distances.erase(distances.begin() + kMaxSoundCount, distances.end());
    }

    // Mark closest sounds as audible
    for (auto &pair : distances) {
        pair.first->setAudible(true);
    }
}

void SceneGraph::refresh() {
    _meshes.clear();
    _lights.clear();
    _emitters.clear();

    for (auto &root : _modelRoots) {
        refreshFromNode(*root);
    }
}

void SceneGraph::refreshFromNode(SceneNode &node) {
    // A disabled node (and its subtree) is skipped from the render lists, so
    // callers can suppress a specific mesh or emitter. The renderer otherwise
    // ignores isEnabled on child nodes.
    if (!node.isEnabled()) {
        return;
    }

    switch (node.type()) {
    case SceneNodeType::Mesh: {
        // Shadow maps use the admitted real geometry, so shadow-only proxy
        // nodes no longer belong in the scene collection.
        auto &modelNode = static_cast<MeshSceneNode &>(node);
        if (modelNode.shouldRender()) {
            _meshes.push_back(&modelNode);
        }
        break;
    }
    case SceneNodeType::Light:
        _lights.push_back(static_cast<LightSceneNode *>(&node));
        break;
    case SceneNodeType::Emitter:
        _emitters.push_back(static_cast<EmitterSceneNode *>(&node));
        break;
    default:
        break;
    }

    for (auto &child : node.children()) {
        refreshFromNode(*child);
    }
}

void SceneGraph::prepareOpaqueLeafs() {
    _opaqueLeafs.clear();
}

void SceneGraph::prepareTransparentLeafs() {
    _transparentLeafs.clear();

    // Add meshes and emitters to transparent leafs
    std::vector<SceneNode *> leafs;
    for (auto &mesh : _meshes) {
        if (mesh->shouldRender() && mesh->isTransparent()) {
            leafs.push_back(mesh);
        }
    }
    // Particles are dropped here rather than at the emitter, so the emitters
    // still simulate and still advance the shared random sequence. A switch
    // that also stopped the simulation would move every later draw's noise and
    // make two builds disagree for a reason that is not what is being compared.
    if (_graphicsOpt.particles) {
        for (auto &emitter : _emitters) {
            for (auto &child : emitter->children()) {
                if (child->type() != SceneNodeType::Particle) {
                    continue;
                }
                auto particle = static_cast<ParticleSceneNode *>(child);
                leafs.push_back(particle);
            }
        }
    }

    // Group transparent leafs into buckets
    SceneNode *bucketParent = nullptr;
    std::vector<SceneNode *> bucket;
    for (auto leaf : leafs) {
        SceneNode *parent = leaf->parent();
        if (leaf->type() == SceneNodeType::Mesh) {
            parent = &static_cast<MeshSceneNode *>(leaf)->model();
        }
        if (!bucket.empty()) {
            int maxCount = 1;
            if (parent->type() == SceneNodeType::Emitter) {
                maxCount = kMaxParticles;
            }
            if (bucketParent != parent || bucket.size() >= maxCount) {
                _transparentLeafs.push_back(std::make_pair(bucketParent, bucket));
                bucket.clear();
            }
        }
        bucketParent = parent;
        bucket.push_back(leaf);
    }
    if (bucketParent && !bucket.empty()) {
        _transparentLeafs.push_back(std::make_pair(bucketParent, bucket));
    }

    // The reference's menu has one transparent mesh even with particles off.
    // Keep this audit behind the existing probe and log once per process: the
    // two requested particle settings then yield directly comparable records.
}

Texture &SceneGraph::render(const glm::ivec2 &dim, SceneOutputAlpha alpha) {
    R_PROFILE_ZONE("SceneGraph::render");
    if (!_renderPipeline) {
        // The mode is what was asked for; the factory decides what the current
        // backend can actually give. There is nothing to reconstruct here any
        // more: the option is the enum. Collapsing a string and a bool at this
        // point is what made --pbr silently inert in the renderer once.
        const RenderMode mode = _graphicsOpt.mode;
        _renderPipeline = _renderPipelineFactory.create(mode, dim, _gpuScene);
        _renderPipeline->init();
        info("Scene '" + _name + "': render pipeline created, mode=" +
                 std::to_string(static_cast<int>(mode)) + " dim=" +
                 std::to_string(dim.x) + "x" + std::to_string(dim.y),
             LogChannel::Graphics);
    }
    auto &pipeline = *_renderPipeline;
    _gpuScene.resetFrame();
    // Handed over beside the rest of the per-frame scene state, and cached
    // behind a dirty flag so a walkmesh-heavy area does not re-walk its faces
    // every frame.
    _gpuScene.setGroundHeight(groundHeight());
    auto cameraNode = this->camera();
    if (cameraNode) {
        auto camera = cameraNode->get().camera();
        auto jitter = computeJitter();
        auto viewProjection = camera->projection() * camera->view();
        if (hasShadowLight()) {
            computeLightSpaceMatrices();
        }
        _graphicsSvc.uniforms.setGlobals([this, &camera, &jitter, &viewProjection](auto &globals) {
            if (jitter != glm::vec2(0.0f)) {
                // Sub-pixel offset in clip space, applied after the projection so
                // that it shifts the raster grid without altering the frustum.
                globals.projection = glm::translate(glm::vec3(jitter, 0.0f)) * camera->projection();
                globals.projectionInv = glm::inverse(globals.projection);
            } else {
                globals.projection = camera->projection();
                globals.projectionInv = camera->projectionInv();
            }
            globals.view = camera->view();
            globals.viewInv = camera->viewInv();
            globals.viewProjection = viewProjection;
            globals.prevViewProjection = _prevViewProjection;
            globals.jitter = glm::vec4(jitter, _prevJitter);
            globals.cameraPosition = glm::vec4(camera->position(), 1.0f);
            globals.worldAmbientColor = glm::vec4(ambientLightColor(), 1.0f);
            globals.clipNear = camera->zNear();
            globals.clipFar = camera->zFar();
            globals.numLights = static_cast<int>(_activeLights.size());
            for (size_t i = 0; i < _activeLights.size(); ++i) {
                auto &light = globals.lights[i];
                light.position = glm::vec4(_activeLights[i]->origin(), _activeLights[i]->isDirectional() ? 0.0f : 1.0f);
                light.color = glm::vec4(_activeLights[i]->color(), 1.0f);
                light.multiplier = _activeLights[i]->multiplier() * _activeLights[i]->strength();
                light.radius = _activeLights[i]->radius();
                light.ambientOnly = static_cast<int>(_activeLights[i]->modelNode().light()->ambientOnly);
                light.dynamicType = _activeLights[i]->modelNode().light()->dynamicType;
                light.shadowCaster = _activeLights[i] == _shadowLight ? 1 : 0;
            }
            if (hasShadowLight()) {
                for (int i = 0; i < kNumShadowLightSpace; ++i) {
                    globals.shadowLightSpace[i] = _shadowLightSpace[i];
                }
                globals.shadowLightPosition = isShadowLightDirectional()
                                                  ? glm::vec4(shadowLightDirection(), 0.0f)
                                                  : glm::vec4(shadowLightPosition(), 1.0f);
                globals.shadowCascadeFarPlanes = _shadowCascadeFarPlanes;
                const float opacity = _graphicsOpt.shadowOpacity >= 0.0f
                                          ? _graphicsOpt.shadowOpacity
                                          : _shadowProperties.opacity;
                globals.shadowStrength = shadowStrength() * opacity;
                globals.shadowRadius = shadowRadius();
            }
            globals.time = _time;
            globals.prevTime = _prevTime;
            if (isFogEnabled()) {
                globals.fogNear = fogNear();
                globals.fogFar = fogFar();
                globals.fogColor = glm::vec4(fogColor(), 1.0f);
            }
        });
        auto halfDim = dim / 2;
        auto screenProjection = glm::mat4(1.0f);
        screenProjection *= glm::scale(glm::vec3(halfDim.x, halfDim.y, 1.0f));
        screenProjection *= glm::translate(glm::vec3(0.5f, 0.5f, 0.0f));
        screenProjection *= glm::scale(glm::vec3(0.5f, 0.5f, 1.0f));
        screenProjection *= camera->projection();
        _graphicsSvc.uniforms.setScreenEffect([&camera, &screenProjection](auto &screenEffect) {
            screenEffect.projection = camera->projection();
            screenEffect.projectionInv = glm::inverse(screenEffect.projection);
            screenEffect.screenProjection = screenProjection;
            screenEffect.clipNear = camera->zNear();
            screenEffect.clipFar = camera->zFar();
        });
        const bool full = !_incrementalSceneReady || _graphicsOpt.admissionForceFull;
        if (full)
            _gpuScene.beginFullCollection();
        collectInto(_gpuScene, full);
        if (full)
            _gpuScene.endFullCollection();
        // A module transition can create grass roots after this frame's graph
        // refresh. Grass alone must not mark incremental admission ready or the
        // following frame would retain only that partial scene. Mesh/transparent
        // lists are rebuilt by refresh and therefore remain the readiness fence.
        const bool hasRenderableLists = !_meshes.empty() || !_transparentLeafs.empty();
        _incrementalSceneReady = hasRenderableLists;
        if (_graphicsOpt.admissionShadow) {
            _shadowGpuScene.resetFrame();
            _shadowGpuScene.beginFullCollection();
            collectInto(_shadowGpuScene, true);
            _shadowGpuScene.endFullCollection();
        }
        // During a module load dynamic nodes can update one frame before the
        // graph has refreshed its render lists. The legacy full walk admitted
        // nothing on that frame; discard those early upserts so both the real
        // and shadow intern histories start at the first complete scene.
        if (!hasRenderableLists) {
            _gpuScene.clear();
            _shadowGpuScene.clear();
        }
    }

    auto shadow = !hasShadowLight()
                      ? RenderShadowKind::None
                      : (isShadowLightDirectional()
                             ? RenderShadowKind::Directional
                             : RenderShadowKind::Point);
    auto &output = pipeline.render(_activeCamera, shadow, alpha);
    snapshotPreviousFrame();
    return output;
}

glm::vec2 SceneGraph::computeJitter() const {
    // Jitter exists when, and only when, something resolves it - which is FSR
    // in the common anti-aliasing slot, in every render mode, because the
    // traced mode's rays derive from this same projection. There is no dial:
    // an override could only ever ask for jitter nothing resolves, and that is
    // shimmer rather than anti-aliasing (measured at 6-12% of pixels changing
    // per frame on a frozen scene). Turning it off means turning the resolver
    // off, which the anti-aliasing option already does.
    if (_graphicsOpt.antialiasing != graphics::AntiAliasing::Fsr) {
        return glm::vec2(0.0f);
    }
    // FSR derives its phase count from the actual render and display widths,
    // not the requested scale. The rounded render extent matters at ratios
    // such as 0.667: repeating a phase one frame early makes the temporal
    // sequence disagree with the resolver that consumes it.
    const glm::ivec2 displaySize {_graphicsOpt.width, _graphicsOpt.height};
    const glm::ivec2 renderSize = graphics::renderExtentFor(_graphicsOpt, displaySize);
    const float ratio = static_cast<float>(displaySize.x) / static_cast<float>(renderSize.x);
    const int kJitterPhases = std::max(
        1, static_cast<int>(8.0f * ratio * ratio));
    auto halton = [](int index, int base) {
        float result = 0.0f;
        float fraction = 1.0f;
        while (index > 0) {
            fraction /= base;
            result += fraction * (index % base);
            index /= base;
        }
        return result;
    };
    int phase = static_cast<int>(_frameIndex % kJitterPhases) + 1;
    glm::vec2 offset {halton(phase, 2) - 0.5f, halton(phase, 3) - 0.5f};
    // Clip-space offset of one RENDER pixel: the jitter shifts the raster grid
    // the scene is actually drawn on, which is the smaller one when upscaling.
    return 2.0f * offset / glm::vec2(renderSize);
}

void SceneGraph::snapshotPreviousFrame() {
    // Latch the camera state of the frame that was just rendered before advancing
    // the frame counter, so that computeJitter still refers to this frame.
    _prevJitter = computeJitter();
    auto cameraNode = this->camera();
    if (cameraNode) {
        auto camera = cameraNode->get().camera();
        _prevViewProjection = camera->projection() * camera->view();
    }

    ++_frameIndex;
    for (auto &mesh : _meshes) {
        mesh->snapshotPreviousFrame(_frameIndex);
    }
    for (auto &[node, leafs] : _opaqueLeafs) {
        node->snapshotPreviousFrame(_frameIndex);
        for (auto &leaf : leafs) {
            leaf->snapshotPreviousFrame(_frameIndex);
        }
    }
    for (auto &[node, leafs] : _transparentLeafs) {
        node->snapshotPreviousFrame(_frameIndex);
        for (auto &leaf : leafs) {
            leaf->snapshotPreviousFrame(_frameIndex);
        }
    }
}

void SceneGraph::collectInto(GpuScene &scene, bool full) {
    R_PROFILE_ZONE("SceneGraph::collectInto");
    if (!_activeCamera) {
        return;
    }

    {
        R_PROFILE_ZONE("SceneGraph::dynamic mesh collection");
        for (auto &mesh : _meshes) {
            // Transparent meshes are registered by their leaf buckets in a full
            // collection. Incremental collection visits only always-dirty streams.
            if (full && (!mesh->shouldRender() || !mesh->isTransparent())) {
                mesh->collectInto(scene);
            } else if (!full && mesh->requiresPerFrameGpuSync() &&
                       !mesh->hasDynamicDeformation()) {
                mesh->collectInto(scene);
            }
        }
    }
    // Grass publishes only its persistent face table. Camera-dependent work is
    // the admission face-band scan and GPU merge expansion.
    {
        R_PROFILE_ZONE("SceneGraph::grass collection");
        for (auto &grass : _grassRoots) {
            if (!grass->isEnabled() || !_graphicsOpt.grass)
                continue;
            if (full)
                grass->collectInto(scene);
            else
                grass->collectIntoIfDirty(scene);
        }
    }

    {
        R_PROFILE_ZONE("SceneGraph::transient collection");
        for (auto &[node, leafs] : _transparentLeafs) {
            if (full) {
                node->collectLeafs(scene, leafs);
            }
        }
    }
    // Flares are collected only when asked for. The billboard they register
    // was unreachable before - RenderCategory::LensFlare is in no admission
    // filter - so this is the switch that decides whether a halo exists at
    // all, in every render mode.
    if (full && _graphicsOpt.lensFlares) {
        for (auto &light : _flareLights) {
            Collision collision;
            if (testLineOfSight(_activeCamera->origin(), light->origin(), collision)) {
                continue;
            }
            light->collectLensFlare(scene, light->modelNode().light()->flares.front());
        }
    }
    if (full)
        scene.checkIdentityStability();
}

static std::vector<glm::vec4> computeFrustumCornersWorldSpace(const glm::mat4 &projection, const glm::mat4 &view) {
    auto inv = glm::inverse(projection * view);

    std::vector<glm::vec4> corners;
    for (auto x = 0; x < 2; ++x) {
        for (auto y = 0; y < 2; ++y) {
            for (auto z = 0; z < 2; ++z) {
                auto pt = inv * glm::vec4(
                                    2.0f * x - 1.0f,
                                    2.0f * y - 1.0f,
                                    static_cast<float>(z),
                                    1.0f);
                corners.push_back(pt / pt.w);
            }
        }
    }

    return corners;
}

static glm::mat4 computeDirectionalLightSpaceMatrix(
    float fov,
    float aspect,
    float near, float far,
    const glm::vec3 &lightDir,
    const glm::mat4 &cameraView,
    int shadowResolution) {

    auto projection = glm::perspectiveRH_ZO(fov, aspect, near, far);

    auto corners = computeFrustumCornersWorldSpace(projection, cameraView);
    glm::vec3 center(0.0f);
    for (auto &v : corners) {
        center += glm::vec3(v);
    }
    center /= corners.size();

    // A frustum AABB changes size when the camera rotates. Its enclosing
    // sphere does not: the far-corner radius below is a function only of the
    // slice distances and lens. Quantising it also absorbs floating-point
    // noise in the lens calculation.
    float halfDepth = 0.5f * (far - near);
    float halfHeight = far * glm::tan(0.5f * fov);
    float halfWidth = aspect * halfHeight;
    float radius = glm::sqrt(halfDepth * halfDepth +
                             halfHeight * halfHeight +
                             halfWidth * halfWidth);
    radius = std::ceil(radius * 16.0f) / 16.0f;

    // Keep the light basis fixed in world space. Building lookAt around the
    // camera-frustum centre would translate the shadow grid continuously.
    const auto up = glm::abs(glm::dot(lightDir, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.99f
                        ? glm::vec3(0.0f, 0.0f, 1.0f)
                        : glm::vec3(0.0f, 1.0f, 0.0f);
    auto lightView = glm::lookAt(glm::vec3(0.0f), lightDir, up);
    auto lightCenter = lightView * glm::vec4(center, 1.0f);

    const float texelSize = (2.0f * radius) /
                            static_cast<float>(shadowResolution);
    lightCenter.x = std::round(lightCenter.x / texelSize) * texelSize;
    lightCenter.y = std::round(lightCenter.y / texelSize) * texelSize;

    const float minX = lightCenter.x - radius;
    const float maxX = lightCenter.x + radius;
    const float minY = lightCenter.y - radius;
    const float maxY = lightCenter.y + radius;
    // Preserve the old ten-radius caster reach, but make it sphere-based too
    // so camera rotation cannot make the depth extent breathe. orthoRH_ZO
    // takes positive near/far distances, hence centre the slice at -10r in
    // view space and cover the resulting [-20r, 0] interval.
    lightView = glm::translate(glm::vec3(
                    0.0f, 0.0f, -10.0f * radius - lightCenter.z)) *
                lightView;

    auto lightProjection = glm::orthoRH_ZO(
        minX, maxX, minY, maxY, 0.0f, 20.0f * radius);
    return lightProjection * lightView;
}

static glm::mat4 getPointLightView(const glm::vec3 &lightPos, CubeMapFace face) {
    switch (face) {
    case CubeMapFace::PositiveX:
        return glm::lookAt(lightPos, lightPos + glm::vec3(1.0, 0.0, 0.0), glm::vec3(0.0, -1.0, 0.0));
    case CubeMapFace::NegativeX:
        return glm::lookAt(lightPos, lightPos + glm::vec3(-1.0, 0.0, 0.0), glm::vec3(0.0, -1.0, 0.0));
    case CubeMapFace::PositiveY:
        return glm::lookAt(lightPos, lightPos + glm::vec3(0.0, 1.0, 0.0), glm::vec3(0.0, 0.0, 1.0));
    case CubeMapFace::NegativeY:
        return glm::lookAt(lightPos, lightPos + glm::vec3(0.0, -1.0, 0.0), glm::vec3(0.0, 0.0, -1.0));
    case CubeMapFace::PositiveZ:
        return glm::lookAt(lightPos, lightPos + glm::vec3(0.0, 0.0, 1.0), glm::vec3(0.0, -1.0, 0.0));
    case CubeMapFace::NegativeZ:
        return glm::lookAt(lightPos, lightPos + glm::vec3(0.0, 0.0, -1.0), glm::vec3(0.0, -1.0, 0.0));
    default:
        throw std::invalid_argument("Invalid cube map face: " + std::to_string(static_cast<int>(face)));
    }
}

void SceneGraph::computeLightSpaceMatrices() {
    if (isShadowLightDirectional()) {
        auto camera = std::static_pointer_cast<PerspectiveCamera>(this->camera()->get().camera());
        // Use the light's authored direction, or the fixed module-room bounds
        // fallback for an identity/default orientation. Neither source follows
        // the camera, so camera motion cannot rotate the shadow projection.
        auto lightDir = shadowLightDirection();
        float fovy = camera->fovy();
        float aspect = camera->aspect();
        float cameraNear = camera->zNear();
        float cameraFar = camera->zFar();
        for (int i = 0; i < kNumShadowCascades; ++i) {
            float far = cameraFar * g_shadowCascadeDivisors[i];
            float near = cameraNear;
            if (i > 0) {
                near = cameraFar * g_shadowCascadeDivisors[i - 1];
            }
            _shadowLightSpace[i] = computeDirectionalLightSpaceMatrix(
                fovy, aspect, near, far, lightDir, camera->view(),
                _graphicsOpt.shadowResolution);
            _shadowCascadeFarPlanes[i] = far;
        }
    } else {
        glm::mat4 projection(glm::perspectiveRH_ZO(kPointLightShadowsFOV, 1.0f, kPointLightShadowsNearPlane, kPointLightShadowsFarPlane));
        for (int i = 0; i < kNumCubeFaces; ++i) {
            glm::mat4 lightView(getPointLightView(shadowLightPosition(), static_cast<CubeMapFace>(i)));
            _shadowLightSpace[i] = projection * lightView;
        }
    }
}

std::vector<LightSceneNode *> SceneGraph::computeClosestLights(int count, const std::function<bool(const LightSceneNode &, float)> &pred) const {
    // Compute distance from each light to the camera
    std::vector<std::pair<LightSceneNode *, float>> distances;
    for (auto &light : _lights) {
        float distance2 = light->getSquareDistanceTo(*_activeCamera);
        if (!pred(*light, distance2)) {
            continue;
        }
        distances.push_back(std::make_pair(light, distance2));
    }

    // Sort lights by distance to the camera. Directional lights are prioritizied
    sort(distances.begin(), distances.end(), [](auto &a, auto &b) {
        auto aLight = a.first;
        auto bLight = b.first;
        if (aLight->isDirectional() && !bLight->isDirectional()) {
            return true;
        }
        if (!aLight->isDirectional() && bLight->isDirectional()) {
            return false;
        }
        float aDistance = a.second;
        float bDistance = b.second;
        return aDistance < bDistance;
    });

    // Keep up to maximum number of lights
    if (distances.size() > count) {
        distances.erase(distances.begin() + count, distances.end());
    }

    std::vector<LightSceneNode *> lights;
    for (auto &light : distances) {
        lights.push_back(light.first);
    }
    return lights;
}

bool SceneGraph::testElevation(const glm::vec3 &position, Collision &outCollision) const {
    static glm::vec3 down(0.0f, 0.0f, -1.0f);

    bool walkable = false;
    float minDistance = std::numeric_limits<float>::max();
    glm::vec3 origin {position.x, position.y, position.z + 0.1f};
    for (auto &root : _walkmeshRoots) {
        if (!root->isEnabled()) {
            continue;
        }
        if (!root->walkmesh().isAreaWalkmesh()) {
            float distance2 = root->getSquareDistanceTo2D(position);
            if (distance2 > kMaxCollisionDistanceWalk2) {
                continue;
            }
        }
        auto objSpaceOrigin = glm::vec3(root->absoluteTransformInverse() * glm::vec4(origin, 1.0f));
        float distance = 0.0f;
        auto face = root->walkmesh().raycast(_walkcheckSurfaces, objSpaceOrigin, down, 2.0f * kElevationTestZ, /*ignoreBackface=*/true, distance);
        if (!face || distance >= minDistance) {
            continue;
        }
        walkable = _walkableSurfaces.count(face->material) > 0;
        if (walkable) {
            outCollision.user = root->user();
            outCollision.intersection = origin + distance * down;
            outCollision.normal = root->absoluteTransform() * glm::vec4 {face->normal, 0.0f};
            outCollision.material = face->material;
        }
        minDistance = distance;
    }

    return walkable;
}

bool SceneGraph::testLineOfSight(const glm::vec3 &origin, const glm::vec3 &dest, Collision &outCollision) const {
    auto originToDest = dest - origin;
    auto dir = glm::normalize(originToDest);
    float maxDistance = glm::length(originToDest);
    float minDistance = std::numeric_limits<float>::max();

    for (auto &root : _walkmeshRoots) {
        if (!root->isEnabled()) {
            continue;
        }
        glm::vec3 originLocal;
        glm::vec3 dirLocal;
        if (root->walkmesh().isAreaWalkmesh()) {
            if (!root->walkmesh().contains(origin) &&
                !root->walkmesh().contains(dest)) {
                continue;
            }
            originLocal = origin;
            dirLocal = dir;
        } else {
            if (root->getSquareDistanceTo(origin) > kMaxCollisionDistanceLineOfSight2) {
                continue;
            }
            originLocal = root->absoluteTransformInverse() * glm::vec4 {origin, 1.0f};
            dirLocal = root->absoluteTransformInverse() * glm::vec4 {dir, 0.0f};
        }
        float distance = 0.0f;
        auto face = root->walkmesh().raycast(_lineOfSightSurfaces, originLocal, dirLocal, maxDistance, /*ignoreBackface=*/false, distance);
        if (!face || distance > minDistance) {
            continue;
        }
        outCollision.user = root->user();
        outCollision.intersection = origin + distance * dir;
        outCollision.normal = root->absoluteTransform() * glm::vec4(face->normal, 0.0f);
        outCollision.material = face->material;
        minDistance = distance;
    }

    return minDistance != std::numeric_limits<float>::max();
}

bool SceneGraph::testWalk(const glm::vec3 &origin, const glm::vec3 &dest, const IUser *excludeUser, Collision &outCollision) const {
    glm::vec3 originToDest(dest - origin);
    glm::vec3 dir(glm::normalize(originToDest));
    float maxDistance = glm::length(originToDest);
    float minDistance = std::numeric_limits<float>::max();

    for (auto &root : _walkmeshRoots) {
        if (!root->isEnabled() || root->user() == excludeUser) {
            continue;
        }
        if (!root->walkmesh().isAreaWalkmesh()) {
            float distance2 = root->getSquareDistanceTo(origin);
            if (distance2 > kMaxCollisionDistanceWalk2) {
                continue;
            }
        }
        glm::vec3 objSpaceOrigin(root->absoluteTransformInverse() * glm::vec4(origin, 1.0f));
        glm::vec3 objSpaceDir(root->absoluteTransformInverse() * glm::vec4(dir, 0.0f));
        float distance = 0.0f;
        auto face = root->walkmesh().raycast(_walkcheckSurfaces, objSpaceOrigin, objSpaceDir, kMaxCollisionDistanceWalk, /*ignoreBackface=*/false, distance);
        if (!face || distance > maxDistance || distance > minDistance) {
            continue;
        }
        outCollision.user = root->user();
        outCollision.intersection = origin + distance * dir;
        outCollision.normal = root->absoluteTransform() * glm::vec4(face->normal, 0.0f);
        outCollision.material = face->material;
        minDistance = distance;
    }

    return minDistance != std::numeric_limits<float>::max();
}

ModelSceneNode *SceneGraph::pickModelAt(int x, int y, IUser *except) const {
    if (!_activeCamera) {
        return nullptr;
    }

    auto camera = _activeCamera->camera();
    glm::vec4 viewport(0.0f, 0.0f, _graphicsOpt.width, _graphicsOpt.height);
    glm::vec3 start(glm::unProjectZO(glm::vec3(x, _graphicsOpt.height - y, 0.0f), camera->view(), camera->projection(), viewport));
    glm::vec3 end(glm::unProjectZO(glm::vec3(x, _graphicsOpt.height - y, 1.0f), camera->view(), camera->projection(), viewport));
    glm::vec3 dir(glm::normalize(end - start));

    std::vector<std::pair<ModelSceneNode *, float>> distances;
    for (auto &model : _modelRoots) {
        if (!model->isPickable() || (except == model->user())) {
            continue;
        }
        if (model->getSquareDistanceTo(start) > kMaxCollisionDistanceLineOfSight2) {
            continue;
        }
        auto objSpaceStart = model->absoluteTransformInverse() * glm::vec4(start, 1.0f);
        auto objSpaceInvDir = 1.0f / (model->absoluteTransformInverse() * glm::vec4(dir, 0.0f));
        float distance;
        if (model->aabb().raycast(objSpaceStart, objSpaceInvDir, kMaxCollisionDistanceLineOfSight, distance) && distance > 0.0f) {
            Collision collision;
            if (testLineOfSight(start, start + distance * dir, collision) && collision.user != model->user()) {
                continue;
            }
            distances.push_back(std::make_pair(model.get(), distance));
        }
    }
    if (distances.empty()) {
        return nullptr;
    }
    sort(distances.begin(), distances.end(), [](auto &left, auto &right) { return left.second < right.second; });

    return distances[0].first;
}

std::optional<std::reference_wrapper<ModelSceneNode>> SceneGraph::pickModelRay(const glm::vec3 &origin, const glm::vec3 &dir) const {
    ModelSceneNode *model {nullptr};
    float minDistance = std::numeric_limits<float>::max();
    for (auto &root : _modelRoots) {
        if (!root->isEnabled() || !root->isPickable()) {
            continue;
        }
        auto aabbWorld = root->aabb() * root->absoluteTransform();
        float distance;
        if (aabbWorld.raycast(origin, 1.0f / dir, std::numeric_limits<float>::max(), distance) &&
            distance < minDistance) {
            model = root.get();
            minDistance = distance;
        }
    }
    if (!model) {
        return std::nullopt;
    }
    return *model;
}

std::shared_ptr<CameraSceneNode> SceneGraph::newCamera() {
    auto node = newSceneNode<CameraSceneNode>();
    return std::move(node);
}

std::shared_ptr<DummySceneNode> SceneGraph::newDummy(ModelNode &modelNode) {
    auto node = newSceneNode<DummySceneNode, ModelNode &>(modelNode);
    return std::move(node);
}

std::shared_ptr<ModelSceneNode> SceneGraph::newModel(Model &model, ModelUsage usage) {
    auto node = newSceneNode<ModelSceneNode, Model &, ModelUsage>(model, usage);
    node->setNameIds({internName(model.name()), 0});
    node->init();
    return std::move(node);
}

std::shared_ptr<WalkmeshSceneNode> SceneGraph::newWalkmesh(Walkmesh &walkmesh) {
    auto node = newSceneNode<WalkmeshSceneNode, Walkmesh &>(walkmesh);
    node->setNameIds({0, internName("walkmesh")});
    return std::move(node);
}

std::shared_ptr<SoundSceneNode> SceneGraph::newSound() {
    auto node = newSceneNode<SoundSceneNode>();
    return std::move(node);
}

std::shared_ptr<MeshSceneNode> SceneGraph::newMesh(ModelSceneNode &model, ModelNode &modelNode) {
    auto node = newSceneNode<MeshSceneNode, ModelSceneNode &, ModelNode &>(model, modelNode);
    node->init();
    return std::move(node);
}

std::shared_ptr<LightSceneNode> SceneGraph::newLight(ModelSceneNode &model, ModelNode &modelNode) {
    auto node = newSceneNode<LightSceneNode, ModelSceneNode &, ModelNode &>(model, modelNode);
    node->init();
    return std::move(node);
}

std::shared_ptr<TriggerSceneNode> SceneGraph::newTrigger(std::vector<glm::vec3> geometry) {
    auto node = newSceneNode<TriggerSceneNode, std::vector<glm::vec3>>(std::move(geometry));
    node->setNameIds({0, internName("trigger")});
    return std::move(node);
}

std::shared_ptr<EmitterSceneNode> SceneGraph::newEmitter(ModelNode &modelNode) {
    auto node = newSceneNode<EmitterSceneNode, ModelNode &>(modelNode);
    node->init();
    return std::move(node);
}

std::shared_ptr<ParticleSceneNode> SceneGraph::newParticle(EmitterSceneNode &emitter) {
    auto node = newSceneNode<ParticleSceneNode, EmitterSceneNode &>(emitter);
    return std::move(node);
}

std::shared_ptr<GrassSceneNode> SceneGraph::newGrass(GrassProperties properties, ModelNode &aabbNode) {
    auto node = newSceneNode<GrassSceneNode, GrassProperties, ModelNode &>(properties, aabbNode);
    node->init();
    return std::move(node);
}

} // namespace scene

} // namespace reone
