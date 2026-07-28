/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <volk.h>

#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/mesh.h"

namespace reone::graphics {
class VulkanRenderer;
class VulkanImage;
class Mesh;
struct GraphicsOptions;
}
namespace reone::scene {
class RenderRegistry;
struct RegisteredSkin;

/** Vulkan-only primary-ray diagnostic. It deliberately owns no raster pass. */
class RayQueryPipeline : boost::noncopyable {
public:
    RayQueryPipeline(graphics::VulkanRenderer &renderer,
                     glm::ivec2 extent,
                     graphics::GraphicsOptions &options);
    ~RayQueryPipeline() { deinit(); }
    void init();
    void deinit();
    void render(VkCommandBuffer cmd, RenderRegistry &registry, uint32_t globalsOffset,
                graphics::VulkanImage &output);

private:
    struct Frame {
        struct Skinned {
            std::unique_ptr<graphics::VulkanBuffer> vertices;
            std::unique_ptr<graphics::VulkanBuffer> storage;
            std::unique_ptr<graphics::VulkanBuffer> scratch;
            VkAccelerationStructureKHR blas {VK_NULL_HANDLE};
        };
        std::unique_ptr<graphics::VulkanBuffer> instances;
        // Kept in exactly TLAS instance order. Query.CommittedInstanceID()
        // indexes this dense array; instanceCustomIndex is a SceneNode id.
        std::unique_ptr<graphics::VulkanBuffer> materials;
        std::unique_ptr<graphics::VulkanBuffer> traceStats;
        std::unique_ptr<graphics::VulkanBuffer> overrides;
        std::unique_ptr<graphics::VulkanBuffer> storage;
        std::unique_ptr<graphics::VulkanBuffer> scratch;
        VkAccelerationStructureKHR tlas {VK_NULL_HANDLE};
        uint32_t capacity {0};
        std::vector<Skinned> skinned;
    };

    graphics::VulkanRenderer &_renderer;
    graphics::GraphicsOptions &_options;
    glm::ivec2 _extent;
    VkDescriptorSetLayout _layout {VK_NULL_HANDLE};
    VkDescriptorPool _pool {VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> _sets {};
    VkPipelineLayout _pipelineLayout {VK_NULL_HANDLE};
    VkPipeline _pipeline {VK_NULL_HANDLE};
    VkDescriptorSetLayout _skinLayout {VK_NULL_HANDLE};
    std::array<VkDescriptorPool, 2> _skinPools {};
    VkPipelineLayout _skinPipelineLayout {VK_NULL_HANDLE};
    VkPipeline _skinPipeline {VK_NULL_HANDLE};
    std::array<Frame, 2> _frames;
    uint32_t _lastInstances {0};
    uint32_t _lastSkinned {0};
    uint32_t _lastDeforming {0};
    uint32_t _lastOutOfRange {0};
    uint32_t _lastEmissive {0};
    uint32_t _lastAdditive {0};
    uint32_t _lastSabers {0};
    uint32_t _lastDangly {0};
    uint32_t _lastSecondaryRays {0};
    uint32_t _lastSecondaryMisses {0};
    uint32_t _lastSurvivingLights {0};
    uint32_t _lastPrimaryHits {0};
    uint32_t _lastShadowRays {0};
    uint32_t _bindlessTextureCapacity {0};
    uint32_t _lastBindlessTextureCount {0};
    /** Must match PushConstants in slang/rayquery.slang. */
    struct TracePushConstants {
        uint32_t frameIndex;
        uint32_t samplesPerPixel;
        float skyIntensity;
        float emissiveIntensity;
        float lightmapIntensity;
        float directIntensity;
        float rayOriginOffset;
        float worldAmbientIntensity;
        float sunIntensity;
        // Bit 0 enables the traceStats counters; must match kTraceFlagStats
        // in slang/rayquery.slang.
        uint32_t traceFlags;
        uint32_t bounceCount;
    };

    /** Must match PushConstants in slang/skin.slang. */
    struct SkinPushConstants {
        uint32_t vertexCount;
        uint32_t vertexStrideFloats;
        int32_t positionOffsetFloats;
        int32_t normalOffsetFloats;
        int32_t boneIndicesOffsetFloats;
        int32_t boneWeightsOffsetFloats;
        int32_t tanSpaceOffsetFloats;
    };

    uint32_t _frameNumber {0};
    bool _inited {false};

    void clearFrame(Frame &frame);
    graphics::VulkanMesh::Geometry skin(VkCommandBuffer cmd,
                                         Frame &frame,
                                         const graphics::VulkanMesh &source,
                                         const graphics::Mesh::VertexLayout &layout,
                                         const RegisteredSkin &skin,
                                         uint32_t globalsOffset);
};
} // namespace reone::scene
