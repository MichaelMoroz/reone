/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <array>
#include <memory>

#include <volk.h>

#include "reone/graphics/rayquery.h"

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
    std::unique_ptr<VulkanPipeline> _compositePipeline;
    glm::vec3 _prevCameraPosition {0.0f};
    bool _temporalHistoryValid {false};
#endif
#ifdef R_ENABLE_FSR
    std::unique_ptr<FsrUpscaler> _fsr;
    std::unique_ptr<VulkanImage> _fsrColor;
    std::unique_ptr<VulkanImage> _fsrOutput;
    std::unique_ptr<VulkanPipeline> _tonemapPipeline;
#endif

    static constexpr int kNumAuxImages = 14;
    std::array<std::array<std::unique_ptr<VulkanImage>, kNumAuxImages>, 2> _auxImages;
    int _lastAuxFrame {-1};

    void clearFrame(Frame &frame);
};

} // namespace reone::graphics
