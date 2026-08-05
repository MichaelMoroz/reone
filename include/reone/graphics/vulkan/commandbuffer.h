/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
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
    void begin(VkCommandBuffer commandBuffer, const VulkanDevice *device) {
        _commandBuffer = commandBuffer;
        _device = device;
    }
    void end() {
        _commandBuffer = VK_NULL_HANDLE;
        _device = nullptr;
    }

    void transitionImage(IImage &image, ImageLayout to) override;
    void beginDebugScope(const char *name, const glm::vec3 &color) override;
    void endDebugScope() override;
    void bindPipeline(Pipeline pipeline) override;
    void bindComputePipeline(Pipeline pipeline) override;
    void bindRayTracingPipeline(Pipeline pipeline) override;
    void bindDescriptorSet(PipelineLayout layout, uint32_t index,
                           DescriptorSet set,
                           const uint32_t *dynamicOffsets,
                           uint32_t dynamicOffsetCount) override;
    void bindComputeDescriptorSet(PipelineLayout layout, uint32_t index,
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
    void pushComputeConstants(PipelineLayout layout, const void *data,
                              uint32_t size) override;
    void pushRayTracingConstants(PipelineLayout layout, const void *data,
                                 uint32_t size) override;
    void dispatch(glm::uvec3 groups) override;
    void clearColor(IImage &image, glm::vec4 color) override;
    void makeGpuSceneSourcesAvailable(const IBuffer &vertices,
                                      const IBuffer &indices) override;
    void publishMergedScene() override;
    void buildSceneTracingStructure(ITracingStructure &structure,
                                    const SceneTracingGeometry &geometry) override;
    void traceRays(Pipeline pipeline, ITracingStructure &structure,
                   glm::uvec2 extent) override;
    void publishTraceOutputForDenoising() override;
    void publishCompositeForUpscaling() override;
    void restoreUpscalerInputsForNextFrame(IImage &color, IImage &depth,
                                           IImage &motion) override;
    void publishUpscaledFrameForTonemapping() override;

    VkCommandBuffer handle() const { return _commandBuffer; }

private:
    VkCommandBuffer _commandBuffer {VK_NULL_HANDLE};
    const VulkanDevice *_device {nullptr};
};

VulkanCommandBuffer &toVulkanCommandBuffer(ICommandBuffer &commandBuffer);

} // namespace graphics

} // namespace reone
