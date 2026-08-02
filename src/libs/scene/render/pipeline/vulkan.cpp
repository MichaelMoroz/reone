/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include "reone/scene/render/pipeline/vulkan.h"

#include <iomanip>
#include <sstream>

#include "reone/graphics/options.h"
#include "reone/graphics/vulkan/rayquery.h"
#include "reone/graphics/vulkan/gpuscene.h"
#include "reone/graphics/vulkan/scenepipeline.h"
#include "reone/scene/render/pipeline/rayquery.h"
#include "reone/system/logutil.h"

namespace reone::scene {

class VulkanRenderPipeline::Callbacks : public graphics::IVulkanSceneCallbacks {
public:
    explicit Callbacks(VulkanRenderPipeline &owner) : _owner(owner) {}

    void renderPrimary(const graphics::VulkanPrimaryRayContext &context) override {
        if (_owner._rayQuery)
            _owner._rayQuery->render(context, std::move(_owner._admissionResult));
    }

    graphics::VulkanGpuScene::View mergeGeometry(VkCommandBuffer commandBuffer) override {
        return _owner._deviceGpuScene->update(commandBuffer,
                                              std::move(_owner._admissionResult.submission.upload));
    }

    std::vector<graphics::VulkanExternalTarget> primaryTargets() const override {
        std::vector<graphics::VulkanExternalTarget> result;
        if (!_owner._rayQuery)
            return result;
        for (const auto &channel : _owner._rayQuery->native().channels())
            result.push_back({channel.name, channel.dumpName, channel.image});
        return result;
    }

private:
    VulkanRenderPipeline &_owner;
};

VulkanRenderPipeline::VulkanRenderPipeline(glm::ivec2 targetSize,
                                           graphics::GraphicsOptions &options,
                                           graphics::VulkanRenderer &renderer,
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

VulkanRenderPipeline::~VulkanRenderPipeline() {
    deinit();
}

void VulkanRenderPipeline::init() {
    if (_inited)
        return;
    _executor = std::make_unique<graphics::VulkanScenePipeline>(
        _targetSize, _options, _renderer, _uniforms, _meshRegistry,
        _textureRegistry, _primaryRayMode);
    _executor->init();
    _deviceGpuScene = std::make_unique<graphics::VulkanGpuScene>();
    _deviceGpuScene->init(_renderer);
    _admission = std::make_unique<GpuSceneAdmission>(_renderer, _options, _gpuScene);
    if (_primaryRayMode) {
        _rayQuery = std::make_unique<RayQueryPipeline>(
            _renderer, _targetSize, _options, _gpuScene, *_deviceGpuScene);
        _rayQuery->init();
    }
    _callbacks = std::make_unique<Callbacks>(*this);
    _inited = true;
}

void VulkanRenderPipeline::deinit() {
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

graphics::Texture &VulkanRenderPipeline::render(const CameraSceneNode *camera) {
    graphics::VulkanSceneFramePlan plan;
    _admissionResult = _admission->prepare(_uniforms.globals().view);
    _lastUploadHash = _admissionResult.uploadHash;
    _lastMaterialReferences =
        _admissionResult.submission.upload.materialReferenceCount;
    _lastMaterialCount =
        static_cast<uint32_t>(_admissionResult.submission.upload.materials.size());
    if (!_primaryRayMode)
        plan.steps.push_back(graphics::VulkanSceneStep::Geometry);
    return _executor->render(plan, *_callbacks);
}

std::vector<RenderTargetInfo> VulkanRenderPipeline::targets() const {
    std::vector<RenderTargetInfo> result;
    for (const auto &target : _executor->targets(*_callbacks)) {
        RenderTargetKind kind = RenderTargetKind::Color;
        switch (target.kind) {
        case graphics::VulkanTargetKind::Depth: kind = RenderTargetKind::Depth; break;
        case graphics::VulkanTargetKind::EyeNormal: kind = RenderTargetKind::EyeNormal; break;
        case graphics::VulkanTargetKind::Motion: kind = RenderTargetKind::Motion; break;
        default: break;
        }
        result.push_back({target.name, kind, nullptr});
    }
    return result;
}

void *VulkanRenderPipeline::renderTargetPreview(const std::string &name,
                                                int mode,
                                                float scale) {
    return _executor->renderTargetPreview(name, mode, scale, *_callbacks);
}

void VulkanRenderPipeline::dumpTargets(const std::filesystem::path &dir) {
    info("Vulkan scene contents: " + formatSceneCounts(_gpuScene.counts()),
         LogChannel::Graphics);
    std::ostringstream hash;
    hash << std::hex << std::setw(16) << std::setfill('0') << _lastUploadHash;
    info("GpuScene upload hash=" + hash.str() +
             ", materials=" + std::to_string(_lastMaterialReferences) + "->" +
             std::to_string(_lastMaterialCount),
         LogChannel::Graphics);
    _executor->dumpTargets(dir, *_callbacks);
}

void VulkanRenderPipeline::restartTemporalHistory() {
    if (_rayQuery)
        _rayQuery->restartTemporalHistory();
}

} // namespace reone::scene
