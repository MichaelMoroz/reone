/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "reone/graphics/vulkan/commandbuffer.h"

#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/buffer.h"

namespace reone {

namespace graphics {

VulkanCommandBuffer &toVulkanCommandBuffer(ICommandBuffer &commandBuffer) {
    auto *result = dynamic_cast<VulkanCommandBuffer *>(&commandBuffer);
    if (!result) {
        throw std::invalid_argument("Command buffer is not implemented by Vulkan");
    }
    return *result;
}

void VulkanCommandBuffer::transitionImage(IImage &image, ImageLayout to) {
    VkImageLayout target;
    switch (to) {
    case ImageLayout::ShaderRead:
        target = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        break;
    case ImageLayout::ColorAttachment:
        target = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        break;
    default:
        throw std::invalid_argument("Unknown RHI image layout");
    }
    toVulkanImage(image).transitionTo(_commandBuffer, target);
}

void VulkanCommandBuffer::bindPipeline(Pipeline pipeline) {
    vkCmdBindPipeline(_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      toVulkanPipeline(pipeline));
}

void VulkanCommandBuffer::bindDescriptorSet(PipelineLayout layout, uint32_t index,
                                             DescriptorSet set,
                                             const uint32_t *dynamicOffsets,
                                             uint32_t dynamicOffsetCount) {
    auto nativeSet = toVulkanDescriptorSet(set);
    vkCmdBindDescriptorSets(_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            toVulkanPipelineLayout(layout), index, 1, &nativeSet,
                            dynamicOffsetCount, dynamicOffsets);
}

void VulkanCommandBuffer::draw(uint32_t vertexCount, uint32_t instanceCount) {
    vkCmdDraw(_commandBuffer, vertexCount, instanceCount, 0, 0);
}

void VulkanCommandBuffer::setScissor(glm::ivec2 offset, glm::uvec2 extent) {
    VkRect2D scissor {};
    scissor.offset = {offset.x, offset.y};
    scissor.extent = {extent.x, extent.y};
    vkCmdSetScissor(_commandBuffer, 0, 1, &scissor);
}

void VulkanCommandBuffer::makeGpuSceneSourcesAvailable(const IBuffer &vertices,
                                                        const IBuffer &indices) {
    const std::array<const IBuffer *, 2> sources {{&vertices, &indices}};
    std::array<VkBufferMemoryBarrier2, 2> barriers {};
    for (size_t i = 0; i < barriers.size(); ++i) {
        auto &barrier = barriers[i];
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barrier.buffer = toVulkanBuffer(*sources[i]).handle();
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
    }
    VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependency.pBufferMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(_commandBuffer, &dependency);
}

void VulkanCommandBuffer::publishMergedScene() {
    VkMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                           VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                            VK_ACCESS_2_SHADER_READ_BIT |
                            VK_ACCESS_2_INDEX_READ_BIT;
    VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(_commandBuffer, &dependency);
}

} // namespace graphics

} // namespace reone
