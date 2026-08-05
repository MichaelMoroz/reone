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

#include "reone/scene/gpuscene.h"

namespace reone::graphics {
class RayQuery;
class IRenderer;
class GpuScene;
class SkyStage;
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
                     graphics::SkyStage &sky);
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
    graphics::SkyStage &_sky;
    std::unique_ptr<graphics::RayQuery> _native;
};

} // namespace reone::scene
