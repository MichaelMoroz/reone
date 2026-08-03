/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/scene/render/pipeline/rayquery.h"

#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/vulkan/rayquery.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/scenepipeline.h"
#include "reone/scene/node/model.h"
#include "reone/scene/render/admission.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone::scene {

RayQueryPipeline::RayQueryPipeline(VulkanRenderer &renderer,
                                   glm::ivec2 extent,
                                   GraphicsOptions &options,
                                   GpuScene &gpuScene,
                                   VulkanGpuScene &deviceGpuScene) :
    _renderer(renderer), _extent(extent), _options(options), _gpuScene(gpuScene),
    _deviceGpuScene(deviceGpuScene) {}

RayQueryPipeline::~RayQueryPipeline() {
    deinit();
}

void RayQueryPipeline::init() {
    if (_native)
        return;
    _native = std::make_unique<VulkanRayQuery>(_renderer, _extent, _options);
    _native->init();
}

void RayQueryPipeline::deinit() {
    _native.reset();
}

void RayQueryPipeline::restartTemporalHistory() {
    if (_native)
        _native->restartTemporalHistory();
}

VulkanRayQuery &RayQueryPipeline::native() {
    return *_native;
}

const VulkanRayQuery &RayQueryPipeline::native() const {
    return *_native;
}

void RayQueryPipeline::render(const VulkanPrimaryRayContext &context,
                              GpuSceneAdmissionResult admission) {
    bool skyBaked = false;
    if (admission.skyRoom) {
        RayQuerySkyRoom bake;
        bake.identity = reinterpret_cast<uint64_t>(admission.skyRoom);
        bake.name = admission.skyRoom->model().name();
        bake.origin = admission.skyOrigin;
        bool valid = true;
        for (const auto &object : _gpuScene.objects()) {
            const auto *mesh = std::get_if<RegisteredMesh>(&object);
            if (!mesh || mesh->cullRoot != admission.skyRoom ||
                !_gpuScene.isObjectEnabled(mesh->id.index)) {
                continue;
            }
            if ((mesh->categories & (renderCategory(RenderCategory::Opaque) |
                                     renderCategory(RenderCategory::Transparent))) == 0) {
                continue;
            }
            if (!std::holds_alternative<std::monostate>(mesh->deformation)) {
                valid = false;
                break;
            }
            const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)];
            if (!texture || !_native->supportsSkyTexture(*texture)) {
                valid = false;
                break;
            }
            bake.meshes.push_back({&mesh->mesh.get(), texture, mesh->transform,
                                   mesh->transformInv, mesh->prevTransform,
                                   mesh->material.uv});
        }
        if (!valid)
            bake.meshes.clear();
        // A valid room with an empty gather is not a failed room - it is a room
        // whose meshes have not activated yet. Loading from a save staggers room
        // visibility, so the first frames here can see zero enabled shell
        // meshes; attempting the bake then would latch bakeSkyRoom's sticky
        // per-room failure and leave the fallback cube - no sky, no sun - for
        // the whole session. Skip the attempt and retry next frame; only a
        // genuinely invalid room (unsupported texture, deforming shell) is
        // handed over empty so the stickiness still applies to it.
        if (valid && bake.meshes.empty()) {
            skyBaked = false;
        } else {
            try {
                skyBaked = _native->bakeSkyRoom(context.commandBuffer, bake);
            } catch (const std::exception &e) {
                warn("Vulkan: sky bake failed for '" + admission.skyRoom->model().name() +
                         "': " + e.what() + "; using fallback cube",
                     LogChannel::Graphics);
            }
        }
    } else {
        _native->clearSkyRoom();
    }

    _native->render(context.commandBuffer, context.globalsOffset, *context.output,
                    context.view, context.projection, context.jitter,
                    std::move(admission.submission), _deviceGpuScene, skyBaked);
}

} // namespace reone::scene
