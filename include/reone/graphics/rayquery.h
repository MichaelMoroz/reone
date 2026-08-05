/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <array>

#include <memory>

#include <optional>

#include <volk.h>

#include "reone/graphics/tracingstructure.h"

#include "reone/graphics/vulkan/buffer.h"

#include <glm/glm.hpp>

#include "reone/graphics/gpuscene.h"

namespace reone::graphics {

class Mesh;
class Texture;
class VulkanImage;
class VulkanPipeline;
class VulkanRenderer;
class FsrUpscaler;
class NrdDenoiser;
struct GraphicsOptions;

struct RayQuerySkyMesh {
    const Mesh *mesh {nullptr};
    const Texture *texture {nullptr};
    glm::mat4 transform {1.0f};
    glm::mat4 transformInv {1.0f};
    glm::mat4 prevTransform {1.0f};
    glm::mat3x4 uv {1.0f};
};

/** Vulkan-free description of the room selected for the fixed sky bake. */
struct RayQuerySkyRoom {
    uint64_t identity {0};
    std::string name;
    glm::vec3 origin {0.0f};
    std::vector<RayQuerySkyMesh> meshes;
};

/** What the scene side hands the tracer for this frame: the upload and its counts. */
struct RayQuerySubmission {
    GpuSceneUpload upload;
    uint32_t dynamicTriangles {0};
    uint32_t skinned {0};
    uint32_t deforming {0};
    uint32_t outOfRange {0};
    uint32_t emissive {0};
    uint32_t additive {0};
    uint32_t sabers {0};
    uint32_t sky {0};
    uint32_t dangly {0};
    uint32_t grass {0};
    uint32_t particles {0};
    uint32_t billboards {0};
};

class RayQuery : boost::noncopyable {
public:
    RayQuery(VulkanRenderer &renderer, glm::ivec2 extent,
                   GraphicsOptions &options);
    ~RayQuery();

    void init();
    void deinit();
    bool bakeSkyRoom(ICommandBuffer &commandBuffer, const RayQuerySkyRoom &room);
    void clearSkyRoom();
    void render(ICommandBuffer &commandBuffer, uint32_t globalsOffset,
                IImage &output, const glm::mat4 &view,
                const glm::mat4 &projection, const glm::vec4 &jitter,
                RayQuerySubmission submission, GpuScene &deviceGpuScene,
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
