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

#include <cstdint>
#include <array>
#include <memory>
#include <unordered_map>

#include "reone/graphics/rendering/rayquery.h"
#include "reone/graphics/rhi/renderer.h"

namespace reone::graphics {

class VulkanTracingPipeline : boost::noncopyable {
public:
    VulkanTracingPipeline(IRenderer &renderer, glm::ivec2 extent,
                          GraphicsOptions &options);
    ~VulkanTracingPipeline();

    void init();
    void deinit();
    std::unique_ptr<ITracingStructure> makeTracingStructure();
    bool bakeSkyRoom(ICommandBuffer &commandBuffer,
                     const RayQuerySkyRoom &room);
    void clearSkyRoom();
    TracingStats render(const TracingPipelineInput &input);

    std::vector<TracingChannel> channels() const;
    void restartTemporalHistory();
    bool supportsSkyTexture(const Texture &texture) const;

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
    uint64_t _skyCubeRoom {0};
    bool _skyCubeReady {false};
    std::unique_ptr<IImage> _skyCube;
    std::array<std::unique_ptr<IImage>, 6> _skyDepth;
    std::unique_ptr<IImage> _skyFallbackCube;

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
    glm::vec3 _prevCameraPosition {0.0f};
    bool _temporalHistoryValid {false};
#endif
#ifdef R_ENABLE_FSR
    std::unique_ptr<ITracingUpscaler> _fsr;
    std::unique_ptr<IImage> _fsrColor;
    std::unique_ptr<IImage> _fsrOutput;
    std::unique_ptr<IComputePipeline> _tonemapPipeline;
    std::vector<ComputeResourceSlot> _tonemapBindings;
#endif

    static constexpr int kNumAuxImages = 14;
    std::array<std::array<std::unique_ptr<IImage>, kNumAuxImages>, 2> _auxImages;
    int _lastAuxFrame {-1};

    void clearFrame(Frame &frame);
};

} // namespace reone::graphics
