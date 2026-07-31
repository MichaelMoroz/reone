/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <volk.h>

#include "reone/graphics/vulkan/buffer.h"
#include "reone/scene/gpuscene.h"

#ifdef R_ENABLE_FSR
#include "reone/scene/render/pipeline/fsrupscaler.h"
#endif
#ifdef R_ENABLE_NRD
#include "reone/scene/render/pipeline/nrddenoiser.h"
#endif

namespace reone::graphics {
class VulkanRenderer;
class VulkanImage;
class Mesh;
struct GraphicsOptions;
}
namespace reone::scene {
class RenderRegistry;
class ModelSceneNode;
struct RegisteredMesh;

/** Vulkan-only primary-ray diagnostic. It deliberately owns no raster pass. */
class RayQueryPipeline : boost::noncopyable {
public:
    RayQueryPipeline(graphics::VulkanRenderer &renderer,
                     glm::ivec2 extent,
                     graphics::GraphicsOptions &options);
    ~RayQueryPipeline() { deinit(); }
    void init();
    void deinit();
    /**
     * View and projection ride along for the denoiser: NRD reprojects from
     * the matrix pair, and only the caller has the camera.
     */
    void render(VkCommandBuffer cmd, RenderRegistry &registry, uint32_t globalsOffset,
                graphics::VulkanImage &output,
                const glm::mat4 &view, const glm::mat4 &projection,
                const glm::vec4 &jitter);

    /** One channel of the trace split, for the render-target viewer and dumps. */
    struct Channel {
        const char *name;
        const char *dumpName;
        graphics::VulkanImage *image;
    };
    /**
     * The split as the frame just rendered left it. This is what makes a claim
     * about a single channel checkable - whether the noise-free target really
     * is free of noise, whether the albedo guide matches the surface - instead
     * of inferring it from the assembled image.
     */
    std::vector<Channel> channels();

    /** Drop the TAA history and NRD's accumulation; the next frame starts cold. */
    void restartTemporalHistory();

private:
    struct Frame {
        std::unique_ptr<graphics::VulkanBuffer> instances;
        std::unique_ptr<graphics::VulkanBuffer> traceStats;
        std::unique_ptr<graphics::VulkanBuffer> blasStorage;
        std::unique_ptr<graphics::VulkanBuffer> tlasStorage;
        std::unique_ptr<graphics::VulkanBuffer> scratch;
        VkAccelerationStructureKHR blas {VK_NULL_HANDLE};
        VkAccelerationStructureKHR tlas {VK_NULL_HANDLE};
        VkDeviceSize blasStorageCapacity {0};
        VkDeviceSize tlasStorageCapacity {0};
        VkDeviceSize scratchCapacity {0};
    };

    graphics::VulkanRenderer &_renderer;
    graphics::GraphicsOptions &_options;
    glm::ivec2 _extent;
    VkDescriptorSetLayout _layout {VK_NULL_HANDLE};
    VkDescriptorPool _pool {VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> _sets {};
    VkPipelineLayout _pipelineLayout {VK_NULL_HANDLE};
    VkPipeline _pipeline {VK_NULL_HANDLE};
    std::unique_ptr<graphics::VulkanBuffer> _raygenSbt;
    VkStridedDeviceAddressRegionKHR _raygenSbtRegion {};
    std::unique_ptr<GpuScene> _gpuScene;
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
    uint32_t _lastSecondaryRays {0};
    uint32_t _lastSecondaryMisses {0};
    uint32_t _lastSurvivingLights {0};
    uint32_t _lastPrimaryHits {0};
    uint32_t _lastShadowRays {0};
    uint32_t _bindlessTextureCapacity {0};
    uint32_t _lastBindlessTextureCount {0};
    /** The detected room baked from its own bounds centre, or null on fallback. */
    const ModelSceneNode *_skyCubeRoom {nullptr};
    bool _skyCubeReady {false};
    std::unique_ptr<graphics::VulkanImage> _skyCube;
    std::array<std::unique_ptr<graphics::VulkanImage>, 6> _skyDepth;
    std::unique_ptr<graphics::VulkanImage> _skyFallbackCube;
    /** Must match PushConstants in slang/rayquery.slang. */
    struct TracePushConstants {
        uint32_t frameIndex;
        uint32_t samplesPerPixel;
        float skyIntensity;
        float emissiveIntensity;
        float lightmapIntensity;
        float directIntensity;
        float rayOriginOffset;
        float sunIntensity;
        // Bit 0 enables the traceStats counters; must match kTraceFlagStats
        // in slang/rayquery.slang.
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
    /** Set by restartTemporalHistory, consumed by the next denoise. */
    bool _restartHistoryRequested {false};
    bool _inited {false};

    /**
     * The NRD denoiser instance, opaque so the header stays NRD-free: the
     * library is an optional local-only toggle (see ENABLE_NRD in the root
     * CMakeLists) and everything referencing it compiles away without it.
     * Null when NRD is not built in or instance creation failed.
     */
    void *_nrdInstance {nullptr};
#ifdef R_ENABLE_NRD
    std::unique_ptr<NrdDenoiser> _nrdDenoiser;
    /** The post-denoise assembly pass; overwrites the trace kernel's write. */
    VkDescriptorSetLayout _compositeLayout {VK_NULL_HANDLE};
    VkDescriptorPool _compositePool {VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> _compositeSets {};
    VkPipelineLayout _compositePipelineLayout {VK_NULL_HANDLE};
    VkPipeline _compositePipeline {VK_NULL_HANDLE};
    /**
     * Camera position last frame, for the teleport check that restarts NRD's
     * accumulation and the upscaler's history together.
     */
    glm::vec3 _prevCameraPosition {0.0f};
    bool _temporalHistoryValid {false};
#endif

#ifdef R_ENABLE_FSR
    /**
     * The upscaler and the two images it needs either side of itself: the
     * composite's linear-HDR handoff, and FSR's resolved output before the
     * display transform. Both single-buffered - FSR keeps its own history
     * internally and neither image outlives the frame that writes it.
     */
    std::unique_ptr<FsrUpscaler> _fsr;
    std::unique_ptr<graphics::VulkanImage> _fsrColor;
    std::unique_ptr<graphics::VulkanImage> _fsrOutput;
    bool _fsrImagesTransitioned {false};
    /** The display transform, moved after the upscaler. */
    VkDescriptorSetLayout _tonemapLayout {VK_NULL_HANDLE};
    VkDescriptorPool _tonemapPool {VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> _tonemapSets {};
    VkPipelineLayout _tonemapPipelineLayout {VK_NULL_HANDLE};
    VkPipeline _tonemapPipeline {VK_NULL_HANDLE};
#endif

    /**
     * The NRD-facing output split, set 2 in the trace pipeline: diffuse and
     * specular radiance with hit distance, normal/roughness, viewZ, motion,
      * the noise-free target, and the two material factors. Written
     * every traced frame whether or not NRD is built in - the channels
     * double as debug views - and double-buffered like every other per-frame
     * resource, since two frames are in flight.
     */
    static constexpr int kNumAuxImages = 10;
    std::array<std::array<std::unique_ptr<graphics::VulkanImage>, kNumAuxImages>, 2> _auxImages;
    VkDescriptorSetLayout _auxLayout {VK_NULL_HANDLE};
    VkDescriptorPool _auxPool {VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> _auxSets {};
    bool _auxImagesTransitioned {false};
    /**
     * Which of the two in-flight copies the last render() wrote. A dump runs
     * after the frame, by which point the renderer's own index may already
     * have moved on to the next one.
     */
    int _lastAuxFrame {-1};

    void clearFrame(Frame &frame);
    std::optional<GpuScene::Admission> classifyMesh(RenderRegistry &registry,
                                                     const RegisteredMesh &mesh,
                                                     const ModelSceneNode *skyRoom,
                                                     bool skyBaked);
    bool bakeSkyRoom(VkCommandBuffer cmd,
                     RenderRegistry &registry,
                     const ModelSceneNode &room,
                     const glm::vec3 &origin);
};
} // namespace reone::scene
