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

namespace reone {

namespace graphics {

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

} // namespace graphics

} // namespace reone
