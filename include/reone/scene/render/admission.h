/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "reone/graphics/rayquery.h"
#include "reone/scene/gpuscene.h"

namespace reone::graphics {
class VulkanRenderer;
struct GraphicsOptions;
} // namespace reone::graphics

namespace reone::scene {

class ModelSceneNode;

struct GpuSceneAdmissionResult {
    graphics::RayQuerySubmission submission;
    const ModelSceneNode *skyRoom {nullptr};
    glm::vec3 skyOrigin {0.0f};
    uint64_t uploadHash {0};
};

/** Owns scene selection, classification, resource ids, and upload lowering. */
class GpuSceneAdmission : boost::noncopyable {
public:
    GpuSceneAdmission(graphics::VulkanRenderer &renderer,
                      graphics::GraphicsOptions &options,
                      GpuScene &gpuScene);

    GpuSceneAdmissionResult prepare(const glm::mat4 &view);

private:
    graphics::VulkanRenderer &_renderer;
    graphics::GraphicsOptions &_options;
    GpuScene &_gpuScene;
    graphics::RayQuerySubmission _submission;
    uint32_t _frameNumber {0};

    std::optional<GpuScene::Classification> classifyMesh(
        const RegisteredMesh &mesh, const ModelSceneNode *skyRoom);
    std::optional<GpuScene::Classification> classifyProcedural(
        const RegisteredProcedural &procedural);
};

} // namespace reone::scene
