/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include <cstdint>
#include <array>
#include <memory>
#include <unordered_map>

#include "reone/graphics/rhi/pipelinecache.h"
#include "reone/graphics/rhi/renderer.h"

namespace reone::graphics {

class TracingPipeline : boost::noncopyable {
public:
    TracingPipeline(IRenderer &renderer, glm::ivec2 extent,
                    GraphicsOptions &options);
    ~TracingPipeline();

    void init();
    void deinit();
    std::unique_ptr<ITracingStructure> makeTracingStructure();
    TracingStats render(const TracingPipelineInput &input);

    std::vector<TracingChannel> channels() const;
    void restartTemporalHistory();

private:
    struct Frame {
        std::unique_ptr<IBuffer> traceStats;
    };

    IRenderer &_renderer;
    GraphicsOptions &_options;
    glm::ivec2 _extent;
    std::unique_ptr<ITracingPipeline> _pipeline;
    std::array<Frame, 2> _frames;
    uint32_t _bindlessTextureCapacity {0};
    uint32_t _lastBindlessTextureCount {0};

    struct TracePushConstants {
        uint32_t frameIndex;
        uint32_t samplesPerPixel;
        float skyIntensity;
        float emissiveIntensity;
        float lightmapIntensity;
        float directIntensity;
        float rayOriginOffset;
        float sunIntensity;
        uint32_t traceFlags;
        uint32_t bounceCount;
        float emitterRadiusRatio;
        float sunAngularRadius;
        float bounceRoughness;
        float roughnessFloor;
        float indirectClamp;
        float exposure;
        uint32_t geometryBase0;
        uint32_t geometryBase1;
        uint32_t skyAvailable;
    };

    bool _restartHistoryRequested {false};
    bool _inited {false};
#ifdef R_ENABLE_NRD
    std::unique_ptr<ITracingDenoiser> _nrdDenoiser;
    std::unique_ptr<IComputePipeline> _compositePipeline;
    std::vector<ComputeResourceSlot> _compositeBindings;
#endif

    static constexpr int kNumAuxImages = 14;
    std::array<std::array<std::unique_ptr<IImage>, kNumAuxImages>, 2> _auxImages;
    int _lastAuxFrame {-1};

    void clearFrame(Frame &frame);
};

} // namespace reone::graphics
