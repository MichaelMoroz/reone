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

#include <volk.h>

#include "reone/graphics/rendering/rayquery.h"
#include "reone/graphics/rhi/computepipeline.h"
#include "reone/graphics/vulkan/descriptorwrites.h"

namespace reone::graphics {

class VulkanImage;
class VulkanPipeline;
class VulkanRenderer;
class FsrUpscaler;
class NrdDenoiser;

class VulkanTracingPipeline : public ITracingPipeline, boost::noncopyable {
public:
    VulkanTracingPipeline(VulkanRenderer &renderer, glm::ivec2 extent,
                          GraphicsOptions &options);
    ~VulkanTracingPipeline();

    void init() override;
    void deinit() override;
    std::unique_ptr<ITracingStructure> makeTracingStructure() override;
    bool bakeSkyRoom(ICommandBuffer &commandBuffer,
                     const RayQuerySkyRoom &room) override;
    void clearSkyRoom() override;
    TracingStats render(const TracingPipelineInput &input) override;

    std::vector<TracingChannel> channels() const override;
    void restartTemporalHistory() override;
    bool supportsSkyTexture(const Texture &texture) const override;

private:
    struct Frame {
        std::unique_ptr<IBuffer> traceStats;
    };

    VulkanRenderer &_renderer;
    GraphicsOptions &_options;
    glm::ivec2 _extent;
    std::unique_ptr<VulkanPipeline> _pipeline;
    std::array<Frame, 2> _frames;
    uint32_t _bindlessTextureCapacity {0};
    uint32_t _lastBindlessTextureCount {0};
    uint64_t _skyCubeRoom {0};
    bool _skyCubeReady {false};
    std::unique_ptr<VulkanImage> _skyCube;
    std::array<std::unique_ptr<VulkanImage>, 6> _skyDepth;
    std::unique_ptr<VulkanImage> _skyFallbackCube;
    std::unordered_map<std::string, DescriptorBinding> _bindings;

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
    void *_nrdInstance {nullptr};
#ifdef R_ENABLE_NRD
    std::unique_ptr<NrdDenoiser> _nrdDenoiser;
    std::unique_ptr<IComputePipeline> _compositePipeline;
    std::vector<ComputeResourceSlot> _compositeBindings;
    glm::vec3 _prevCameraPosition {0.0f};
    bool _temporalHistoryValid {false};
#endif
#ifdef R_ENABLE_FSR
    std::unique_ptr<FsrUpscaler> _fsr;
    std::unique_ptr<VulkanImage> _fsrColor;
    std::unique_ptr<VulkanImage> _fsrOutput;
    std::unique_ptr<IComputePipeline> _tonemapPipeline;
    std::vector<ComputeResourceSlot> _tonemapBindings;
#endif

    static constexpr int kNumAuxImages = 14;
    std::array<std::array<std::unique_ptr<VulkanImage>, kNumAuxImages>, 2> _auxImages;
    int _lastAuxFrame {-1};

    void clearFrame(Frame &frame);
    const DescriptorBinding &binding(const char *name) const;
};

} // namespace reone::graphics
