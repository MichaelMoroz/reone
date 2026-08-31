/*
 * Copyright (c) 2026 The reone project contributors
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
#pragma once

#include "reone/graphics/rendering/rayquery.h"
#include "reone/scene/gpuscene.h"

namespace reone::graphics {
class IRenderer;
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
    GpuSceneAdmission(graphics::IRenderer &renderer,
                      graphics::GraphicsOptions &options,
                      GpuScene &gpuScene);

    GpuSceneAdmissionResult prepare(
        const glm::mat4 &view,
        graphics::GpuSceneUpload reuse = {});

private:
    graphics::IRenderer &_renderer;
    graphics::GraphicsOptions &_options;
    GpuScene &_gpuScene;
    graphics::RayQuerySubmission _submission;
    uint32_t _frameNumber {0};
    uint64_t _admissionGeneration {1};
    uint64_t _resourceGeneration {UINT64_MAX};
    uint64_t _optionsFingerprint {0};
    const ModelSceneNode *_classifiedSkyRoom {nullptr};
    uint64_t _skyCacheGeneration {0};
    const ModelSceneNode *_cachedSkyRoom {nullptr};
    glm::vec3 _cachedSkyOrigin {0.0f};

    std::optional<GpuScene::Classification> classifyMesh(
        const RegisteredMesh &mesh, const ModelSceneNode *skyRoom);
    std::optional<GpuScene::Classification> classifyProcedural(
        const RegisteredProcedural &procedural);
    void rebuildSubmissionCounts(const ModelSceneNode *skyRoom);
};

} // namespace reone::scene
