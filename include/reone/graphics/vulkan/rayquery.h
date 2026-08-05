/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <volk.h>

#include "reone/graphics/rayquery.h"
#include "reone/graphics/tracingstructure.h"
#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/gpuscene.h"

#ifdef R_ENABLE_FSR
#include "reone/graphics/vulkan/fsrupscaler.h"
#endif
#ifdef R_ENABLE_NRD
#include "reone/graphics/vulkan/nrddenoiser.h"
#endif

namespace reone::graphics {

class VulkanRenderer;
class VulkanImage;
class VulkanPipeline;
class Texture;
struct GraphicsOptions;

/** Owns the complete native primary-ray path and consumes scene-lowered data. */
class VulkanRayQuery : boost::noncopyable {
public:
    VulkanRayQuery(VulkanRenderer &renderer, glm::ivec2 extent,
                   GraphicsOptions &options);
    ~VulkanRayQuery();

    void init();
    void deinit();
    bool bakeSkyRoom(ICommandBuffer &commandBuffer, const RayQuerySkyRoom &room);
    void clearSkyRoom();
    void render(ICommandBuffer &commandBuffer, uint32_t globalsOffset,
                IImage &output, const glm::mat4 &view,
                const glm::mat4 &projection, const glm::vec4 &jitter,
                RayQuerySubmission submission, VulkanGpuScene &deviceGpuScene,
                bool skyBaked);

    struct Channel {
        const char *name;
        const char *dumpName;
        VulkanImage *image;
    };
    std::vector<Channel> channels();
    void restartTemporalHistory();
    std::optional<uint32_t> textureId(const Texture &texture) const;
    bool supportsSkyTexture(const Texture &texture) const;

private:
    struct Frame {
        std::unique_ptr<VulkanBuffer> traceStats;
        std::unique_ptr<ITracingStructure> tracingStructure;
    };

    VulkanRenderer &_renderer;
    GraphicsOptions &_options;
    glm::ivec2 _extent;
    std::unique_ptr<VulkanPipeline> _pipeline;
    std::array<Frame, 2> _frames;
    uint32_t _lastInstances {0};
    uint32_t _lastTriangles {0};
    uint32_t _lastDynamicTriangles {0};
    uint32_t _lastSkinned {0};
    uint32_t _lastDeforming {0};
    uint32_t _lastOutOfRange {0};
    uint32_t _lastEmissive {0};
    uint32_t _lastAdditive {0};
    uint32_t _lastSabers {0};
    uint32_t _lastSky {0};
    uint32_t _lastDangly {0};
    uint32_t _lastGrass {0};
    uint32_t _lastParticles {0};
    uint32_t _lastBillboards {0};
    uint32_t _lastSecondaryRays {0};
    uint32_t _lastSecondaryMisses {0};
    uint32_t _lastSurvivingLights {0};
    uint32_t _lastPrimaryHits {0};
    uint32_t _lastShadowRays {0};
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

    uint32_t _frameNumber {0};
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
