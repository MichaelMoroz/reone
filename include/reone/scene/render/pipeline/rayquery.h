/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "reone/graphics/rayquery.h"
#include "reone/scene/gpuscene.h"

namespace reone::graphics {
class VulkanRayQuery;
class VulkanRenderer;
struct GraphicsOptions;
struct VulkanPrimaryRayContext;
} // namespace reone::graphics

namespace reone::scene {

class ModelSceneNode;
struct RegisteredMesh;
struct RegisteredGrass;
struct RegisteredParticles;
struct RegisteredBillboard;

/** Scene admission and curated lowering for the native primary-ray path. */
class RayQueryPipeline : boost::noncopyable {
public:
    RayQueryPipeline(graphics::VulkanRenderer &renderer,
                     glm::ivec2 extent,
                     graphics::GraphicsOptions &options,
                     GpuScene &gpuScene);
    ~RayQueryPipeline();

    void init();
    void deinit();
    void render(const graphics::VulkanPrimaryRayContext &context);
    void restartTemporalHistory();
    graphics::VulkanRayQuery &native();
    const graphics::VulkanRayQuery &native() const;

private:
    graphics::VulkanRenderer &_renderer;
    glm::ivec2 _extent;
    graphics::GraphicsOptions &_options;
    GpuScene &_gpuScene;
    std::unique_ptr<graphics::VulkanRayQuery> _native;
    graphics::RayQuerySubmission _submission;
    uint32_t _frameNumber {0};

    std::optional<GpuScene::Classification> classifyMesh(
        const RegisteredMesh &mesh, const ModelSceneNode *skyRoom, bool skyBaked);
    std::optional<GpuScene::Classification> classifyGrass(const RegisteredGrass &grass);
    std::optional<GpuScene::Classification> classifyParticles(const RegisteredParticles &particles);
    std::optional<GpuScene::Classification> classifyBillboard(const RegisteredBillboard &billboard);
};

} // namespace reone::scene
