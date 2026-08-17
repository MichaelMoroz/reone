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

#include "reone/scene/node/light.h"

#include "reone/graphics/di/services.h"
#include "reone/graphics/material.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/textures.h"
#include "reone/scene/graph.h"
#include "reone/scene/node/camera.h"
#include "reone/scene/render/pipeline.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

static constexpr float kFadeSpeed = 2.0f;
static constexpr float kMinDirectionalLightRadius = 100.0f;

void LightSceneNode::init() {
    _modelNode.vectorValueAtTime(ControllerTypes::color, 0.0f, _color);
    _modelNode.floatValueAtTime(ControllerTypes::radius, 0.0f, _radius);
    _modelNode.floatValueAtTime(ControllerTypes::multiplier, 0.0f, _multiplier);
}

void LightSceneNode::update(float dt) {
    SceneNode::update(dt);

    // Fading
    bool fading = _modelNode.light()->fading;
    if (_active) {
        if (fading) {
            _strength = glm::min(1.0f, _strength + kFadeSpeed * dt);
        } else {
            _strength = 1.0f;
        }
    } else {
        if (fading) {
            _strength = glm::max(0.0f, _strength - kFadeSpeed * dt);
        } else {
            _strength = 0.0f;
        }
    }
}

void LightSceneNode::collectLensFlare(GpuScene &scene, const ModelNode::LensFlare &flare) {
    std::shared_ptr<Camera> camera(_sceneGraph.camera()->get().camera());
    if (!camera) {
        return;
    }
    auto texture = _resourceSvc.textures.get(flare.textureName);
    if (!texture) {
        return;
    }
    // A flare has its own authored tint; flare-only lights need not carry a
    // light-colour controller, whose default would turn the billboard black.
    auto color = glm::vec4(flare.colorShift, 0.5f);
    auto transform = glm::translate(origin());
    // The authored flare size is a SCREEN size, not a world one.
    //
    // The reference draws a flare by projecting its origin to clip space,
    // dividing through, and offsetting the quad's corners in NDC - so
    // 0.2 * flare.size is a fraction of the viewport and the halo stays the
    // same size however far away the light is. Handing that number to a
    // world-space quad instead gives a card 0.2 units across, which at any real
    // distance is a few pixels: four flares registered on Dantooine and moved
    // 32 pixels between them.
    //
    // The merged quad is built from world-space right/up vectors and cannot be
    // told to work in NDC without a per-kind branch in the merge, so the world
    // size that yields the intended NDC size is computed here instead. A world
    // offset h at distance d projects to an NDC offset h * P[i][i] / d, so
    // inverting that gives the size below - and because it scales with
    // distance, the halo holds its screen size exactly as the reference's does.
    const float ndcSize = 0.2f * flare.size;
    const glm::mat4 &projection = camera->projection();
    const float distance = std::max(0.01f, glm::length(origin() - camera->position()));
    const glm::vec2 size {
        ndcSize * distance / std::max(1e-4f, std::abs(projection[0][0])),
        ndcSize * distance / std::max(1e-4f, std::abs(projection[1][1]))};
    // Transparent, not LensFlare: every admission filter admits Opaque and
    // Transparent only, so a billboard registered under LensFlare is dropped
    // before it can be classified. A flare is an additive transparent
    // billboard, which is what classifyProcedural makes of it from here.
    scene.addBillboard(renderCategory(RenderCategory::Transparent),
                         id(), nameIds(), *texture, color, transform, glm::inverse(transform), size, &_model);
}

bool LightSceneNode::isDirectional() const {
    return _radius >= kMinDirectionalLightRadius;
}

bool LightSceneNode::hasAuthoredDirection() const {
    // The binary model format always supplies a quaternion, including the
    // identity default used when no direction was authored. Treat a transform
    // that leaves the conventional light forward axis unchanged as absent so
    // the scene-centre fallback can provide an azimuth.
    constexpr auto defaultDirection = glm::vec3(0.0f, 0.0f, -1.0f);
    return glm::dot(direction(), defaultDirection) < 0.9999f;
}

glm::vec3 LightSceneNode::direction() const {
    // KotOR cameras and lights face along local -Z. The complete scene-node
    // transform retains the light node's authored orientation and every parent
    // transform, while w=0 deliberately excludes translation.
    auto direction = glm::vec3(absoluteTransform() * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    if (glm::length2(direction) < glm::epsilon<float>()) {
        return glm::vec3(0.0f, 0.0f, -1.0f);
    }
    return glm::normalize(direction);
}

} // namespace scene

} // namespace reone
