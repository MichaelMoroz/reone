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

#include "reone/graphics/vulkan/commandbuffer.h"

#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/tracingstructure.h"
#include "reone/graphics/vulkan/pipeline.h"

#include <array>

namespace reone {

namespace graphics {

VulkanCommandBuffer &toVulkanCommandBuffer(ICommandBuffer &commandBuffer) {
    auto *result = dynamic_cast<VulkanCommandBuffer *>(&commandBuffer);
    if (!result) {
        throw std::invalid_argument("Command buffer is not implemented by Vulkan");
    }
    return *result;
}

CommandBufferDebugScope::CommandBufferDebugScope(ICommandBuffer &commandBuffer,
                                                  const char *name,
                                                  const glm::vec3 &color) :
    _commandBuffer(commandBuffer) {
    _commandBuffer.beginDebugScope(name, color);
}

CommandBufferDebugScope::~CommandBufferDebugScope() {
    _commandBuffer.endDebugScope();
}

void VulkanCommandBuffer::beginDebugScope(const char *name, const glm::vec3 &color) {
    if (_device) {
        _device->beginLabel(_commandBuffer, name, color);
    }
}

void VulkanCommandBuffer::endDebugScope() {
    if (_device) {
        _device->endLabel(_commandBuffer);
    }
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
    case ImageLayout::DepthAttachment:
        target = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        break;
    case ImageLayout::DepthRead:
        target = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        break;
    case ImageLayout::General:
        target = VK_IMAGE_LAYOUT_GENERAL;
        break;
    default:
        throw std::invalid_argument("Unknown RHI image layout");
    }
    toVulkanImage(image).transitionTo(_commandBuffer, target);
}

void VulkanCommandBuffer::transitionImages(const std::vector<IImage *> &images,
                                           ImageLayout to) {
    VkImageLayout target;
    switch (to) {
    case ImageLayout::ShaderRead:
        target = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        break;
    case ImageLayout::ColorAttachment:
        target = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        break;
    case ImageLayout::DepthAttachment:
        target = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        break;
    case ImageLayout::DepthRead:
        target = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        break;
    case ImageLayout::General:
        target = VK_IMAGE_LAYOUT_GENERAL;
        break;
    default:
        throw std::invalid_argument("Unknown RHI image layout");
    }

    std::vector<VulkanImage *> nativeImages;
    nativeImages.reserve(images.size());
    for (auto *image : images) {
        nativeImages.push_back(&toVulkanImage(*image));
    }
    VulkanImage::transitionTo(_commandBuffer, nativeImages, target);
}

void VulkanCommandBuffer::bindPipeline(Pipeline pipeline) {
    vkCmdBindPipeline(_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      toVulkanPipeline(pipeline));
}

void VulkanCommandBuffer::bindRayTracingPipeline(Pipeline pipeline) {
    vkCmdBindPipeline(_commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      toVulkanPipeline(pipeline));
}

void VulkanCommandBuffer::bindComputePipeline(Pipeline pipeline) {
    vkCmdBindPipeline(_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      toVulkanPipeline(pipeline));
}

void VulkanCommandBuffer::bindComputeDescriptorSet(PipelineLayout layout, uint32_t index,
                                                    DescriptorSet set,
                                                    const uint32_t *dynamicOffsets,
                                                    uint32_t dynamicOffsetCount) {
    auto nativeSet = toVulkanDescriptorSet(set);
    vkCmdBindDescriptorSets(_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            toVulkanPipelineLayout(layout), index, 1, &nativeSet,
                            dynamicOffsetCount, dynamicOffsets);
}

void VulkanCommandBuffer::pushComputeConstants(PipelineLayout layout, const void *data,
                                                uint32_t size) {
    vkCmdPushConstants(_commandBuffer, toVulkanPipelineLayout(layout),
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, size, data);
}

void VulkanCommandBuffer::dispatchCompute(glm::uvec3 groups) {
    vkCmdDispatch(_commandBuffer, groups.x, groups.y, groups.z);
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

void VulkanCommandBuffer::bindRayTracingDescriptorSet(PipelineLayout layout, uint32_t index,
                                                       DescriptorSet set,
                                                       const uint32_t *dynamicOffsets,
                                                       uint32_t dynamicOffsetCount) {
    auto nativeSet = toVulkanDescriptorSet(set);
    vkCmdBindDescriptorSets(_commandBuffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
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

static VkImageLayout toVulkanImageLayout(ImageLayout layout) {
    switch (layout) {
    case ImageLayout::ShaderRead: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case ImageLayout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case ImageLayout::DepthAttachment: return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    case ImageLayout::DepthRead: return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    case ImageLayout::General: return VK_IMAGE_LAYOUT_GENERAL;
    }
    throw std::invalid_argument("Unknown RHI image layout");
}

void VulkanCommandBuffer::beginRendering(glm::ivec2 extent,
                                         const std::vector<RenderAttachment> &colors,
                                         const RenderAttachment *depthAttachment,
                                         uint32_t viewMask, bool invertedViewport) {
    std::vector<VkRenderingAttachmentInfo> nativeColors(colors.size());
    for (size_t i = 0; i < colors.size(); ++i) {
        const auto &source = colors[i];
        auto &target = nativeColors[i];
        target.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        target.imageView = toVulkanImageView(source.view);
        target.imageLayout = toVulkanImageLayout(source.layout);
        target.loadOp = source.load == AttachmentLoad::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                        source.load == AttachmentLoad::Load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        target.storeOp = source.store == AttachmentStore::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if (source.clear.integer)
            target.clearValue.color.uint32[0] = source.clear.uintValue;
        else
            target.clearValue.color = {{source.clear.color.r, source.clear.color.g, source.clear.color.b, source.clear.color.a}};
    }
    VkRenderingAttachmentInfo nativeDepth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depthAttachment) {
        nativeDepth.imageView = toVulkanImageView(depthAttachment->view);
        nativeDepth.imageLayout = toVulkanImageLayout(depthAttachment->layout);
        nativeDepth.loadOp = depthAttachment->load == AttachmentLoad::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR :
                             depthAttachment->load == AttachmentLoad::Load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        nativeDepth.storeOp = depthAttachment->store == AttachmentStore::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        nativeDepth.clearValue.depthStencil = {depthAttachment->clear.depth, 0};
    }
    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y)};
    rendering.layerCount = 1;
    rendering.viewMask = viewMask;
    rendering.colorAttachmentCount = static_cast<uint32_t>(nativeColors.size());
    rendering.pColorAttachments = nativeColors.data();
    rendering.pDepthAttachment = depthAttachment ? &nativeDepth : nullptr;
    VkViewport viewport {0.0f, invertedViewport ? static_cast<float>(extent.y) : 0.0f,
                         static_cast<float>(extent.x), invertedViewport ? -static_cast<float>(extent.y) : static_cast<float>(extent.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y)}};
    vkCmdBeginRendering(_commandBuffer, &rendering);
    vkCmdSetViewport(_commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(_commandBuffer, 0, 1, &scissor);
}

void VulkanCommandBuffer::endRendering() { vkCmdEndRendering(_commandBuffer); }

void VulkanCommandBuffer::bindIndexBuffer(const IBuffer &buffer, uint64_t offset) {
    vkCmdBindIndexBuffer(_commandBuffer, toVulkanBuffer(buffer).handle(), offset, VK_INDEX_TYPE_UINT32);
}

void VulkanCommandBuffer::drawIndexed(uint32_t indexCount, uint32_t firstIndex,
                                      uint32_t instanceCount) {
    vkCmdDrawIndexed(_commandBuffer, indexCount, instanceCount, firstIndex, 0, 0);
}

void VulkanCommandBuffer::pushGraphicsConstants(PipelineLayout layout, const void *data,
                                                uint32_t size) {
    // Both stages, and they must match the range the layout declares exactly:
    // for every byte pushed, the stage flags here have to include every stage
    // in the overlapping range, so pushing to a subset of what the layout says
    // is a validation error rather than a narrowing.
    //
    // The vertex half exists for the shadow pass, whose vertex stage has to
    // know which caster's transforms to read. Nothing else reads push
    // constants from a vertex shader, and nothing has to - declaring the range
    // in both stages costs a pipeline layout nothing.
    vkCmdPushConstants(_commandBuffer, toVulkanPipelineLayout(layout),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, size, data);
}

void VulkanCommandBuffer::pushRayTracingConstants(PipelineLayout layout, const void *data,
                                                   uint32_t size) {
    vkCmdPushConstants(_commandBuffer, toVulkanPipelineLayout(layout),
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, size, data);
}

void VulkanCommandBuffer::dispatch(IComputePipeline &shader, glm::uvec3 groups,
                                   const ComputeBindingSet &bindings,
                                   const ComputeBindingSet *overrides,
                                   const void *pushConstants, uint32_t pushConstantSize) {
    toVulkanComputePipeline(shader).dispatch(_commandBuffer, _frameIndex, groups,
                                             bindings, overrides,
                                             pushConstants, pushConstantSize);
}

void VulkanCommandBuffer::clearColor(IImage &image, glm::vec4 color) {
    VkClearColorValue clear {{color.r, color.g, color.b, color.a}};
    VkImageSubresourceRange range {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(_commandBuffer, toVulkanImage(image).handle(),
                          VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
}

namespace {

struct ResourceUse {
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

ResourceUse toVulkanBufferUse(BufferUse use) {
    switch (use) {
    case BufferUse::TransferWrite:
        return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case BufferUse::ComputeRead:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT};
    case BufferUse::ComputeWrite:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT};
    case BufferUse::ComputeReadWrite:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT};
    case BufferUse::AccelerationStructureBuildRead:
        return {VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    case BufferUse::HostRead:
        return {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
    case BufferUse::ShaderRead:
        return {VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT};
    case BufferUse::IndexRead:
        return {VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT};
    }
    throw std::invalid_argument("Unknown buffer use");
}

ResourceUse toVulkanImageUse(ImageUse use) {
    switch (use) {
    case ImageUse::RayTracingStore:
        return {VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case ImageUse::ComputeRead:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    case ImageUse::ComputeSample:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case ImageUse::ComputeStore:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    case ImageUse::ComputeStorageRead:
        return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    }
    throw std::invalid_argument("Unknown image use");
}

} // namespace

void VulkanCommandBuffer::bufferBarrier(IBuffer &buffer, BufferUse from, BufferUse to) {
    const auto source = toVulkanBufferUse(from);
    const auto destination = toVulkanBufferUse(to);
    VkBufferMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = source.stage;
    barrier.srcAccessMask = source.access;
    barrier.dstStageMask = destination.stage;
    barrier.dstAccessMask = destination.access;
    barrier.buffer = toVulkanBuffer(buffer).handle();
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(_commandBuffer, &dependency);
}

void VulkanCommandBuffer::imageBarrier(IImage &image, ImageUse from, ImageUse to) {
    const auto source = toVulkanImageUse(from);
    const auto destination = toVulkanImageUse(to);
    VkImageMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = source.stage;
    barrier.srcAccessMask = source.access;
    barrier.dstStageMask = destination.stage;
    barrier.dstAccessMask = destination.access;
    barrier.oldLayout = from == ImageUse::ComputeSample
                            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.image = toVulkanImage(image).handle();
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(_commandBuffer, &dependency);
}

void VulkanCommandBuffer::prepareSceneTracingStructure(
    ITracingStructure &structure, const SceneTracingGeometry &geometry) {
    toVulkanTracingStructure(structure).prepare(geometry);
}

void VulkanCommandBuffer::buildSceneTracingStructure(
    ITracingStructure &structure, const SceneTracingGeometry &geometry) {
    toVulkanTracingStructure(structure).build(_commandBuffer, geometry);
}

void VulkanCommandBuffer::traceRays(Pipeline pipeline, ITracingStructure &structure,
                                    glm::uvec2 extent) {
    toVulkanTracingStructure(structure).traceRays(_commandBuffer,
                                                   toVulkanPipeline(pipeline), extent);
}

} // namespace graphics

} // namespace reone
