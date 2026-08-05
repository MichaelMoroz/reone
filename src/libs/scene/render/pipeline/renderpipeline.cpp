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
#include "reone/scene/render/pipeline/renderpipeline.h"

#include <iomanip>
#include <sstream>

#include "reone/graphics/options.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/graphics/rendering/rayquery.h"
#include "reone/graphics/rendering/gpuscene.h"
#include "reone/graphics/rendering/scenepipeline.h"
#include "reone/scene/render/pipeline/rayquery.h"
#include "reone/system/logutil.h"

namespace reone::scene {

class RenderPipeline::Callbacks : public graphics::ISceneCallbacks {
public:
    explicit Callbacks(RenderPipeline &owner) : _owner(owner) {}

    void renderPrimary(const graphics::PrimaryRayContext &context) override {
        if (_owner._rayQuery)
            _owner._rayQuery->render(context, std::move(_owner._admissionResult));
    }

    graphics::GpuScene::View mergeGeometry(graphics::ICommandBuffer &commandBuffer) override {
        return _owner._deviceGpuScene->update(commandBuffer,
                                              _owner._admissionResult.submission.upload);
    }

    std::vector<graphics::ExternalTarget> primaryTargets() const override {
        std::vector<graphics::ExternalTarget> result;
        if (!_owner._rayQuery)
            return result;
        for (const auto &channel : _owner._rayQuery->native().channels())
            result.push_back({channel.name, channel.dumpName, channel.image});
        return result;
    }

private:
    RenderPipeline &_owner;
};

RenderPipeline::RenderPipeline(glm::ivec2 targetSize,
                                           graphics::GraphicsOptions &options,
                                           graphics::IRenderer &renderer,
                                           graphics::Uniforms &uniforms,
                                           graphics::IMeshRegistry &meshRegistry,
                                           graphics::TextureRegistry &textureRegistry,
                                           GpuScene &gpuScene,
                                           bool primaryRayMode) :
    _targetSize(std::move(targetSize)),
    _options(options),
    _renderer(renderer),
    _uniforms(uniforms),
    _meshRegistry(meshRegistry),
    _textureRegistry(textureRegistry),
    _gpuScene(gpuScene),
    _primaryRayMode(primaryRayMode) {
}

RenderPipeline::~RenderPipeline() {
    deinit();
}

void RenderPipeline::init() {
    if (_inited)
        return;
    _executor = std::make_unique<graphics::ScenePipeline>(
        _targetSize, _options, _renderer, _uniforms, _meshRegistry,
        _textureRegistry, _primaryRayMode);
    _executor->init();
    _deviceGpuScene = std::make_unique<graphics::GpuScene>();
    _deviceGpuScene->init(_renderer);
    _admission = std::make_unique<GpuSceneAdmission>(_renderer, _options, _gpuScene);
    if (_primaryRayMode) {
        _rayQuery = std::make_unique<RayQueryPipeline>(
            _renderer, _targetSize, _options, _gpuScene);
        _rayQuery->init();
    }
    _callbacks = std::make_unique<Callbacks>(*this);
    _inited = true;
}

void RenderPipeline::deinit() {
    if (!_inited)
        return;
    _callbacks.reset();
    if (_rayQuery)
        _rayQuery->deinit();
    _rayQuery.reset();
    _admission.reset();
    if (_deviceGpuScene)
        _deviceGpuScene->deinit();
    _deviceGpuScene.reset();
    _executor.reset();
    _inited = false;
}

graphics::Texture &RenderPipeline::render(const CameraSceneNode *camera,
                                                RenderShadowKind shadow) {
    graphics::SceneFramePlan plan;
    switch (shadow) {
    case RenderShadowKind::Directional:
        plan.shadow = graphics::SceneShadow::Directional;
        break;
    case RenderShadowKind::Point:
        plan.shadow = graphics::SceneShadow::Point;
        break;
    default:
        plan.shadow = graphics::SceneShadow::None;
        break;
    }
    auto uploadArena = std::move(_admissionResult.submission.upload);
    _admissionResult = _admission->prepare(
        _uniforms.globals().view, std::move(uploadArena));
    _lastUploadHash = _admissionResult.uploadHash;
    _lastMaterialReferences =
        _admissionResult.submission.upload.materialReferenceCount;
    _lastMaterialCount =
        static_cast<uint32_t>(_admissionResult.submission.upload.materials.size());
    if (_primaryRayMode) {
        plan.steps.push_back(graphics::SceneStep::Geometry);
    } else {
        plan.steps.push_back(graphics::SceneStep::ProcessPBRTextures);
        plan.steps.push_back(graphics::SceneStep::Shadow);
        plan.steps.push_back(graphics::SceneStep::Geometry);
        if (_options.pbr)
            plan.steps.push_back(graphics::SceneStep::PBRResolve);
        else
            plan.steps.push_back(graphics::SceneStep::RetroResolve);
        // Transparency composites onto the resolved image, so it follows
        // whichever resolve ran. Raster only: in the traced mode additive
        // sprites belong to the march and drawing them here would double them.
        plan.steps.push_back(graphics::SceneStep::Blended);
    }
    return _executor->render(plan, *_callbacks);
}

std::vector<RenderTargetInfo> RenderPipeline::targets() const {
    std::vector<RenderTargetInfo> result;
    for (const auto &target : _executor->targets(*_callbacks)) {
        RenderTargetKind kind = RenderTargetKind::Color;
        switch (target.kind) {
        case graphics::TargetKind::Depth: kind = RenderTargetKind::Depth; break;
        case graphics::TargetKind::EyeNormal: kind = RenderTargetKind::EyeNormal; break;
        case graphics::TargetKind::Motion: kind = RenderTargetKind::Motion; break;
        default: break;
        }
        result.push_back({target.name, kind, nullptr});
    }
    return result;
}

void *RenderPipeline::renderTargetPreview(const std::string &name,
                                                int mode,
                                                float scale) {
    return _executor->renderTargetPreview(name, mode, scale, *_callbacks);
}

void RenderPipeline::dumpTargets(const std::filesystem::path &dir) {
    info("Scene contents: " + formatSceneCounts(_gpuScene.counts()),
         LogChannel::Graphics);
    std::ostringstream hash;
    hash << std::hex << std::setw(16) << std::setfill('0') << _lastUploadHash;
    info("GpuScene upload hash=" + hash.str() +
             ", materials=" + std::to_string(_lastMaterialReferences) + "->" +
             std::to_string(_lastMaterialCount),
         LogChannel::Graphics);
    uint64_t grassClusters = 0;
    for (const auto &range : _admissionResult.submission.upload.grassRanges)
        grassClusters += range.clusterCount;
    info("GpuScene grass faces=" +
             std::to_string(_admissionResult.submission.upload.grassFaces.size()) +
             ", in_band_faces=" +
             std::to_string(_admissionResult.submission.upload.grassRanges.size()) +
             ", clusters=" + std::to_string(grassClusters),
         LogChannel::Graphics);
    _executor->dumpTargets(dir, *_callbacks);
}

void RenderPipeline::restartTemporalHistory() {
    if (_rayQuery)
        _rayQuery->restartTemporalHistory();
}

} // namespace reone::scene
