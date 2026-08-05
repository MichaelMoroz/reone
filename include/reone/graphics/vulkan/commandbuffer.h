/*
 * Copyright (c) 2020-2026 The reone project contributors
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

#include "reone/graphics/rhi/commandbuffer.h"

#include "rhi.h"

namespace reone {

namespace graphics {

class VulkanImage;
class VulkanDevice;

class VulkanCommandBuffer : public ICommandBuffer {
public:
    void begin(VkCommandBuffer commandBuffer, const VulkanDevice *device, uint32_t frameIndex) {
        _commandBuffer = commandBuffer;
        _device = device;
        _frameIndex = frameIndex;
    }
    void end() {
        _commandBuffer = VK_NULL_HANDLE;
        _device = nullptr;
        _frameIndex = 0;
    }

    void transitionImage(IImage &image, ImageLayout to) override;
    void transitionImages(const std::vector<IImage *> &images, ImageLayout to) override;
    void beginDebugScope(const char *name, const glm::vec3 &color) override;
    void endDebugScope() override;
    void bindPipeline(Pipeline pipeline) override;
    void bindRayTracingPipeline(Pipeline pipeline) override;
    void bindDescriptorSet(PipelineLayout layout, uint32_t index,
                           DescriptorSet set,
                           const uint32_t *dynamicOffsets,
                           uint32_t dynamicOffsetCount) override;
    void bindRayTracingDescriptorSet(PipelineLayout layout, uint32_t index,
                                     DescriptorSet set,
                                     const uint32_t *dynamicOffsets,
                                     uint32_t dynamicOffsetCount) override;
    void draw(uint32_t vertexCount, uint32_t instanceCount) override;
    void setScissor(glm::ivec2 offset, glm::uvec2 extent) override;
    void beginRendering(glm::ivec2 extent,
                        const std::vector<RenderAttachment> &colors,
                        const RenderAttachment *depth,
                        uint32_t viewMask,
                        bool invertedViewport) override;
    void endRendering() override;
    void bindIndexBuffer(const IBuffer &buffer, uint64_t offset) override;
    void drawIndexed(uint32_t indexCount, uint32_t firstIndex) override;
    void pushFragmentConstants(PipelineLayout layout, const void *data,
                               uint32_t size) override;
    void pushRayTracingConstants(PipelineLayout layout, const void *data,
                                 uint32_t size) override;
    void dispatch(IComputePipeline &shader, glm::uvec3 groups,
                  const ComputeBindingSet &bindings,
                  const ComputeBindingSet *overrides,
                  const void *pushConstants, uint32_t pushConstantSize) override;
    void clearColor(IImage &image, glm::vec4 color) override;
    void bufferBarrier(IBuffer &buffer, BufferUse from, BufferUse to) override;
    void imageBarrier(IImage &image, ImageUse from, ImageUse to) override;
    void buildSceneTracingStructure(ITracingStructure &structure,
                                    const SceneTracingGeometry &geometry) override;
    void traceRays(Pipeline pipeline, ITracingStructure &structure,
                   glm::uvec2 extent) override;

    VkCommandBuffer handle() const { return _commandBuffer; }
    uint32_t frameIndex() const { return _frameIndex; }

private:
    VkCommandBuffer _commandBuffer {VK_NULL_HANDLE};
    const VulkanDevice *_device {nullptr};
    uint32_t _frameIndex {0};
};

VulkanCommandBuffer &toVulkanCommandBuffer(ICommandBuffer &commandBuffer);

} // namespace graphics

} // namespace reone
