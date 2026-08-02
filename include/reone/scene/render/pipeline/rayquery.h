/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "reone/scene/gpuscene.h"

namespace reone::graphics {
class VulkanRayQuery;
class VulkanRenderer;
class VulkanGpuScene;
struct GraphicsOptions;
struct VulkanPrimaryRayContext;
} // namespace reone::graphics

namespace reone::scene {

struct GpuSceneAdmissionResult;

/** Sky bake and native primary-ray execution for one admitted upload. */
class RayQueryPipeline : boost::noncopyable {
public:
    RayQueryPipeline(graphics::VulkanRenderer &renderer,
                     glm::ivec2 extent,
                     graphics::GraphicsOptions &options,
                     GpuScene &gpuScene,
                     graphics::VulkanGpuScene &deviceGpuScene);
    ~RayQueryPipeline();

    void init();
    void deinit();
    void render(const graphics::VulkanPrimaryRayContext &context,
                GpuSceneAdmissionResult admission);
    void restartTemporalHistory();
    graphics::VulkanRayQuery &native();
    const graphics::VulkanRayQuery &native() const;

private:
    graphics::VulkanRenderer &_renderer;
    glm::ivec2 _extent;
    graphics::GraphicsOptions &_options;
    GpuScene &_gpuScene;
    graphics::VulkanGpuScene &_deviceGpuScene;
    std::unique_ptr<graphics::VulkanRayQuery> _native;
};

} // namespace reone::scene
