/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <volk.h>

#include "reone/graphics/vulkan/buffer.h"

namespace reone::graphics {
class VulkanRenderer;
class VulkanImage;
struct GraphicsOptions;
}
namespace reone::scene {
class RenderRegistry;

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
        std::unique_ptr<graphics::VulkanBuffer> instances;
        // Kept in exactly TLAS instance order. Query.CommittedInstanceID()
        // indexes this dense array; instanceCustomIndex is a SceneNode id.
        std::unique_ptr<graphics::VulkanBuffer> materials;
        std::unique_ptr<graphics::VulkanBuffer> traceStats;
        std::unique_ptr<graphics::VulkanBuffer> storage;
        std::unique_ptr<graphics::VulkanBuffer> scratch;
        VkAccelerationStructureKHR tlas {VK_NULL_HANDLE};
        uint32_t capacity {0};
    };

    graphics::VulkanRenderer &_renderer;
    graphics::GraphicsOptions &_options;
    glm::ivec2 _extent;
    VkDescriptorSetLayout _layout {VK_NULL_HANDLE};
    VkDescriptorPool _pool {VK_NULL_HANDLE};
    std::array<VkDescriptorSet, 2> _sets {};
    VkPipelineLayout _pipelineLayout {VK_NULL_HANDLE};
    VkPipeline _pipeline {VK_NULL_HANDLE};
    std::array<Frame, 2> _frames;
    uint32_t _lastInstances {0};
    uint32_t _lastDeforming {0};
    uint32_t _lastOutOfRange {0};
    uint32_t _lastEmissive {0};
    uint32_t _lastSecondaryRays {0};
    uint32_t _lastSecondaryMisses {0};
    /** Must match PushConstants in slang/rayquery.slang. */
    struct TracePushConstants {
        uint32_t frameIndex;
        uint32_t samplesPerPixel;
    };

    uint32_t _frameNumber {0};
    bool _inited {false};

    void clearFrame(Frame &frame);
};
} // namespace reone::scene
