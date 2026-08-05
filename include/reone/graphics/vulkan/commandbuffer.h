/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "reone/graphics/commandbuffer.h"

#include "rhi.h"

namespace reone {

namespace graphics {

class VulkanImage;

class VulkanCommandBuffer : public ICommandBuffer {
public:
    void begin(VkCommandBuffer commandBuffer) { _commandBuffer = commandBuffer; }
    void end() { _commandBuffer = VK_NULL_HANDLE; }

    void transitionImage(IImage &image, ImageLayout to) override;
    void bindPipeline(Pipeline pipeline) override;
    void bindDescriptorSet(PipelineLayout layout, uint32_t index,
                           DescriptorSet set,
                           const uint32_t *dynamicOffsets,
                           uint32_t dynamicOffsetCount) override;
    void draw(uint32_t vertexCount, uint32_t instanceCount) override;
    void setScissor(glm::ivec2 offset, glm::uvec2 extent) override;

    VkCommandBuffer handle() const { return _commandBuffer; }

private:
    VkCommandBuffer _commandBuffer {VK_NULL_HANDLE};
};

} // namespace graphics

} // namespace reone
