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

#include <memory>

#include "reone/graphics/rhi/pipelinecache.h"

namespace reone::graphics {

class IRenderer;

/** The path-tracing scene consumer. Its native implementation stays behind
 * the RHI, while scene policy and lifetime live with the other render clients. */
class TracingPipeline : boost::noncopyable {
public:
    TracingPipeline(IRenderer &renderer, glm::ivec2 extent, GraphicsOptions &options);
    ~TracingPipeline();

    void init();
    void deinit();
    std::unique_ptr<ITracingStructure> makeTracingStructure();
    bool bakeSkyRoom(ICommandBuffer &commandBuffer, const RayQuerySkyRoom &room);
    void clearSkyRoom();
    bool supportsSkyTexture(const Texture &texture) const;
    TracingStats render(const TracingPipelineInput &input);
    void restartTemporalHistory();
    std::vector<TracingChannel> channels() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace reone::graphics
