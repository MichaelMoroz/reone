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

#include "model.h"

#include <cmath>

#include "reone/game/types.h"
#include "reone/graphics/animation.h"
#include "reone/graphics/di/module.h"
#include "reone/graphics/format/mdlmdxreader.h"
#include "reone/graphics/format/tgawriter.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/uniforms.h"
#include "reone/resource/di/module.h"
#include "reone/resource/exception/notfound.h"
#include "reone/resource/resources.h"
#include "reone/scene/di/module.h"
#include "reone/scene/graphs.h"
#include "reone/system/clock.h"
#include "reone/system/di/module.h"
#include "reone/system/logutil.h"
#include "reone/system/stream/memoryinput.h"
#include "reone/system/stream/fileoutput.h"

using namespace reone::game;
using namespace reone::graphics;
using namespace reone::scene;
using namespace reone::resource;

namespace reone {

void ModelResourceViewModel::initScene() {
    auto &graphs = _sceneSvc.graphs();
    graphs.reserve(kSceneMain);
    auto &scene = graphs.get(kSceneMain);
    _cameraNode = scene.newCamera();
    scene.setActiveCamera(_cameraNode.get());
    scene.setAmbientLightColor(glm::vec3(1.0f));
}

void ModelResourceViewModel::openModel(const ResourceId &id, IInputStream &mdl) {
    auto mdxRes = _resourceSvc.resources().find(ResourceId(id.resRef, ResType::Mdx));
    if (!mdxRes) {
        throw ResourceNotFoundException("Companion MDX resource not found: " + id.resRef.value());
    }
    auto mdx = MemoryInputStream(mdxRes->data);
    auto reader = MdlMdxReader(mdl, mdx, _graphicsModule.statistic());
    reader.load();

    auto &scene = _sceneSvc.graphs().get(kSceneMain);
    scene.clear();
    scene.update(0.0f);
    _modelNode.reset();

    _model = reader.model();
    if (!_model->superModelName().empty()) {
        auto superModel = _resourceSvc.models().get(_model->superModelName());
        _model->setSuperModel(std::move(superModel));
    }
    _model->init();
    _animations = _model->getAnimationNames();

    _modelNode = scene.newModel(*_model, ModelUsage::Creature);
    _modelHeading = 0.0f;
    _modelPitch = 0.0f;
    updateModelTransform();
    scene.addRoot(_modelNode);

    // Use a valid provisional aspect until the canvas supplies its actual size
    // on the first render.
    frameCamera(1.0f);
    _cameraNeedsFraming = true;
}

void ModelResourceViewModel::update3D() {
    auto ticks = _systemSvc.clock().millis();
    if (_lastTicks == 0) {
        _lastTicks = ticks;
    }
    float delta = (ticks - _lastTicks) / 1000.0f;
    _lastTicks = ticks;

    update3D(delta);
}

void ModelResourceViewModel::update3D(float delta) {
    auto &scene = _sceneSvc.graphs().get(kSceneMain);
    scene.update(delta);

    AnimationProgress progress;
    if (_modelNode && !_modelNode->animationChannels().empty()) {
        const auto &animChannel = _modelNode->animationChannels().front();
        if (animChannel.anim) {
            progress.playing = true;
            progress.time = animChannel.time;
            progress.duration = animChannel.lipAnim ? animChannel.lipAnim->length() : animChannel.anim->length();
        }
    }
    _animationProgress = std::move(progress);
}

void ModelResourceViewModel::render3D(int w, int h, const std::filesystem::path *capturePath) {
    // The wx page is made visible before openResource has finished loading the
    // engine and model. Its first paint must not dereference the as-yet-unset
    // external renderer; the idle refresh after openModel will draw it.
    if (!_modelNode) {
        return;
    }

    if (w <= 0 || h <= 0) {
        return;
    }

    float aspect = w / static_cast<float>(h);
    if (_cameraNeedsFraming) {
        frameCamera(aspect);
        _cameraNeedsFraming = false;
    }
    _cameraNode->setPerspectiveProjection(glm::radians(55.0f), aspect, kDefaultClipPlaneNear, kDefaultClipPlaneFar);

    auto &renderer = _graphicsModule.renderer();
    // The renderer presents the completed frame to the wxWidgets child window.
    renderer.beginFrame(glm::ivec2(w, h));
    // Scene pipelines open render passes of their own, so they must record
    // after the frame starts but before the renderer opens its 2D composite.
    auto &scene = _sceneSvc.graphs().get(kSceneMain);
    auto &output = scene.render(glm::ivec2(w, h));
    renderer.presentSceneOutput(output);
    if (capturePath) {
        // captureFrame reads this child canvas's swapchain, not the whole
        // toolkit window. Its RGB8 output is already what TgaWriter expects.
        auto stream = FileOutputStream(*capturePath);
        TgaWriter(renderer.captureFrame()).save(stream);
        info("Wrote preview screenshot: " + capturePath->string());
    }
    renderer.endFrame();
}

void ModelResourceViewModel::playAnimation(std::string anim, std::shared_ptr<graphics::LipAnimation> lipAnim) {
    if (!_modelNode) {
        return;
    }
    _modelNode->playAnimation(anim, std::move(lipAnim), AnimationProperties::fromFlags(AnimationFlags::loop));
    _animationPlaying = true;
}

void ModelResourceViewModel::pauseAnimation() {
    if (!_modelNode) {
        return;
    }
    _modelNode->pauseAnimation();
    _animationPlaying = false;
}

void ModelResourceViewModel::resumeAnimation() {
    if (!_modelNode) {
        return;
    }
    _modelNode->resumeAnimation();
    _animationPlaying = true;
}

void ModelResourceViewModel::setAnimationTime(float time) {
    if (!_modelNode) {
        return;
    }
    _modelNode->setAnimationTime(time);
}

void ModelResourceViewModel::updateModelTransform() {
    auto transform = glm::mat4(1.0f);
    transform *= glm::rotate(_modelHeading, glm::vec3(0.0f, 0.0f, 1.0f));
    transform *= glm::rotate(_modelPitch, glm::vec3(-1.0f, 0.0f, 0.0f));
    _modelNode->setLocalTransform(transform);
}

void ModelResourceViewModel::updateCameraTransform() {
    auto cameraTransform = glm::mat4(1.0f);
    cameraTransform = glm::translate(cameraTransform, _cameraPosition);
    cameraTransform *= glm::rotate(glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    cameraTransform *= glm::rotate(glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    _cameraNode->setLocalTransform(cameraTransform);
}

void ModelResourceViewModel::frameCamera(float aspect) {
    constexpr float kVerticalFov = glm::radians(55.0f);
    constexpr float kFrameMargin = 1.12f;
    constexpr float kFallbackDistance = 8.0f;

    _cameraTarget = glm::vec3(0.0f);
    _cameraFramingDistance = kFallbackDistance;

    const auto &aabb = _model->aabb();
    const glm::vec3 min = aabb.min();
    const glm::vec3 max = aabb.max();
    const glm::vec3 extent = max - min;
    const bool validBounds = !aabb.isDegenerate() &&
        std::isfinite(min.x) && std::isfinite(min.y) && std::isfinite(min.z) &&
        std::isfinite(max.x) && std::isfinite(max.y) && std::isfinite(max.z) &&
        extent.x >= 0.0f && extent.y >= 0.0f && extent.z >= 0.0f;

    if (validBounds) {
        // The camera sits at target + (0, d, 0) and looks back along -Y, and
        // heading rotates the model about Z. So Z is screen-up and invariant
        // under heading, while X and Y trade places as the model spins: their
        // combined radius is what stays constant, and it serves as both the
        // half-width and the half-depth.
        const float halfHeight = 0.5f * extent.z;
        const float halfRadius = 0.5f * glm::length(glm::vec2(extent.x, extent.y));
        const float tanV = std::tan(kVerticalFov * 0.5f);
        const float tanH = tanV * glm::max(aspect, 0.01f);
        // Fit each axis to its own field of view, then step back by the radius
        // because the model reaches that far toward the camera from its centre.
        const float distance = kFrameMargin * glm::max(halfHeight / tanV, halfRadius / tanH) + halfRadius;
        if (std::isfinite(halfHeight) && std::isfinite(halfRadius) &&
            (halfHeight > 0.001f || halfRadius > 0.001f) &&
            std::isfinite(distance) && distance > kDefaultClipPlaneNear &&
            distance < kDefaultClipPlaneFar * 0.9f) {
            _cameraTarget = 0.5f * (min + max);
            _cameraFramingDistance = distance;
        }
    }

    _cameraPosition = _cameraTarget + glm::vec3(0.0f, _cameraFramingDistance, 0.0f);
    updateCameraTransform();
}

void ModelResourceViewModel::onGLCanvasMouseMotion(int x, int y, bool leftDown, bool rightDown) {
    int dx = x - _lastMouseX;
    int dy = y - _lastMouseY;

    if (leftDown) {
        _modelHeading += dx / glm::pi<float>() / 64.0f;
        //_modelPitch += dy / glm::pi<float>() / 64.0f;
        updateModelTransform();
    } else if (rightDown) {
        float distance = glm::max(_cameraPosition.y - _cameraTarget.y, kDefaultClipPlaneNear);
        float panScale = distance / 2048.0f;
        _cameraPosition.x += dx * panScale;
        _cameraPosition.z += dy * panScale;
        updateCameraTransform();
    }

    _lastMouseX = x;
    _lastMouseY = y;
}

void ModelResourceViewModel::onGLCanvasMouseWheel(int delta) {
    float distance = _cameraPosition.y - _cameraTarget.y;
    float step = glm::max(_cameraFramingDistance * 0.1f, kDefaultClipPlaneNear);
    float minDistance = kDefaultClipPlaneNear * 1.1f;
    distance = glm::max(minDistance, distance - glm::clamp(delta, -1, 1) * step);
    _cameraPosition.y = _cameraTarget.y + distance;
    updateCameraTransform();
}

} // namespace reone
