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
#include "reone/scene/render/pipeline/rayquery.h"

#include "reone/graphics/options.h"
#include "reone/graphics/rendering/rayquery.h"
#include "reone/graphics/rendering/sky.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/graphics/rendering/scenepipeline.h"
#include "reone/scene/render/admission.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone::scene {

RayQueryPipeline::RayQueryPipeline(IRenderer &renderer,
                                    glm::ivec2 extent,
                                    GraphicsOptions &options) :
    _renderer(renderer), _extent(extent), _options(options) {}

RayQueryPipeline::~RayQueryPipeline() {
    deinit();
}

void RayQueryPipeline::init() {
    if (_native)
        return;
    _native = std::make_unique<RayQuery>(_renderer, _extent, _options);
    _native->init();
}

void RayQueryPipeline::deinit() {
    _native.reset();
}

void RayQueryPipeline::restartTemporalHistory() {
    if (_native)
        _native->restartTemporalHistory();
}

RayQuery &RayQueryPipeline::native() {
    return *_native;
}

const RayQuery &RayQueryPipeline::native() const {
    return *_native;
}

void RayQueryPipeline::render(const PrimaryRayContext &context,
                              GpuSceneAdmissionResult admission,
                              const SkyBinding &sky) {
    // The bake that produced this binding ran in RenderPipeline, which drives
    // it for every mode. The tracer only consumes the cube.
    _native->render(*context.commandBuffer, context.globalsOffset, *context.output,
                    context.view, context.projection, context.jitter,
                    std::move(admission.submission), context.scene, sky);
}

} // namespace reone::scene
