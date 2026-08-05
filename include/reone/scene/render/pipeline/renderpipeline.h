/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "../admission.h"
#include "../pipeline.h"

namespace reone::graphics {
class IMeshRegistry;
class Uniforms;
class IRenderer;
class ScenePipeline;
class GpuScene;
} // namespace reone::graphics

namespace reone::scene {

class RayQueryPipeline;

/** Scene-side frame ordering and draw selection for the scene executor. */
class RenderPipeline : public IRenderPipeline, boost::noncopyable {
public:
    RenderPipeline(glm::ivec2 targetSize,
                         graphics::GraphicsOptions &options,
                         graphics::IRenderer &renderer,
                         graphics::Uniforms &uniforms,
                         graphics::IMeshRegistry &meshRegistry,
                         graphics::TextureRegistry &textureRegistry,
                         GpuScene &gpuScene,
                         bool primaryRayMode = false);
    ~RenderPipeline();

    void init() override;
    void deinit();
    graphics::Texture &render(const CameraSceneNode *camera,
                              RenderShadowKind shadow) override;
    std::vector<RenderTargetInfo> targets() const override;
    void *renderTargetPreview(const std::string &name, int mode, float scale) override;
    void dumpTargets(const std::filesystem::path &dir) override;
    void restartTemporalHistory() override;

private:
    class Callbacks;

    glm::ivec2 _targetSize;
    graphics::GraphicsOptions &_options;
    graphics::IRenderer &_renderer;
    graphics::Uniforms &_uniforms;
    graphics::IMeshRegistry &_meshRegistry;
    graphics::TextureRegistry &_textureRegistry;
    GpuScene &_gpuScene;
    bool _primaryRayMode {false};
    bool _inited {false};
    std::unique_ptr<graphics::ScenePipeline> _executor;
    std::unique_ptr<graphics::GpuScene> _deviceGpuScene;
    std::unique_ptr<GpuSceneAdmission> _admission;
    GpuSceneAdmissionResult _admissionResult;
    std::unique_ptr<RayQueryPipeline> _rayQuery;
    uint64_t _lastUploadHash {0};
    uint32_t _lastMaterialReferences {0};
    uint32_t _lastMaterialCount {0};
    std::unique_ptr<Callbacks> _callbacks;
};

} // namespace reone::scene
