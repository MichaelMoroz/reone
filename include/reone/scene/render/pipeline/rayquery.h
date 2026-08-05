/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "reone/scene/gpuscene.h"

namespace reone::graphics {
class RayQuery;
class IRenderer;
class GpuScene;
struct GraphicsOptions;
struct PrimaryRayContext;
} // namespace reone::graphics

namespace reone::scene {

struct GpuSceneAdmissionResult;

/** Sky bake and native primary-ray execution for one admitted upload. */
class RayQueryPipeline : boost::noncopyable {
public:
    RayQueryPipeline(graphics::IRenderer &renderer,
                     glm::ivec2 extent,
                     graphics::GraphicsOptions &options,
                     GpuScene &gpuScene,
                     graphics::GpuScene &deviceGpuScene);
    ~RayQueryPipeline();

    void init();
    void deinit();
    void render(const graphics::PrimaryRayContext &context,
                GpuSceneAdmissionResult admission);
    void restartTemporalHistory();
    graphics::RayQuery &native();
    const graphics::RayQuery &native() const;

private:
    graphics::IRenderer &_renderer;
    glm::ivec2 _extent;
    graphics::GraphicsOptions &_options;
    GpuScene &_gpuScene;
    graphics::GpuScene &_deviceGpuScene;
    std::unique_ptr<graphics::RayQuery> _native;
};

} // namespace reone::scene
