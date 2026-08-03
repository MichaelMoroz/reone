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

#include "reone/graphics/vulkan/gbuffer.h"

#include "reone/graphics/vulkan/device.h"

namespace reone {

namespace graphics {

std::vector<VkFormat> VulkanGBuffer::colorFormats() {
    // Matching the retained targets: RGBA8 for the colour-ish channels, RGBA8
    // for eye normals (GL uses RGB8, but three-component render targets are not
    // universally supported and the fourth channel costs nothing here), and
    // RG16F for motion, which needs the range and the sign.
    return {
        VK_FORMAT_R8G8B8A8_UNORM, // Diffuse
        VK_FORMAT_R8G8B8A8_UNORM, // EyeNormal
        VK_FORMAT_R8G8B8A8_UNORM, // Lightmap
        VK_FORMAT_R8G8B8A8_UNORM, // SelfIllum
        VK_FORMAT_R16G16_SFLOAT,  // Motion
        VK_FORMAT_R8G8B8A8_UNORM, // MaterialAmbient
        VK_FORMAT_R8G8B8A8_UNORM  // MaterialDiffuse
    };
}

void VulkanGBuffer::init(glm::ivec2 extent) {
    _extent = extent;
    auto formats = colorFormats();
    for (int i = 0; i < Count; ++i) {
        _color[i] = std::make_unique<VulkanImage>(_device);
        _color[i]->initColorAttachment(extent, formats[i]);
    }
    _depth = std::make_unique<VulkanImage>(_device);
    _depth->initDepth(extent, depthFormat());

    // Images are created UNDEFINED and dynamic rendering does not transition
    // them, so the first frame would begin a pass declaring a layout the images
    // are not in. Move them once here; the per-frame cycle takes over after
    // that.
    _device.immediateSubmit([this](VkCommandBuffer cmd) {
        std::array<VkImageMemoryBarrier2, Count + 1> barriers {};
        for (int i = 0; i < Count; ++i) {
            auto &b = barriers[i];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.image = _color[i]->handle();
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;
        }
        auto &d = barriers[Count];
        d.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        d.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        d.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
        d.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        d.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        d.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        d.image = _depth->handle();
        d.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        d.subresourceRange.levelCount = 1;
        d.subresourceRange.layerCount = 1;

        VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    _colorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    _depthLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
}

void VulkanGBuffer::deinit() {
    for (auto &image : _color) {
        image.reset();
    }
    _depth.reset();
}

/**
 * The stage and access a layout implies.
 *
 * Naming them per layout rather than per call site is what keeps a barrier
 * honest in both directions: the same transition run backwards at the start of
 * the next frame needs the masks swapped, and hardcoding one direction quietly
 * under-synchronises the other.
 */
static void scopeForLayout(VkImageLayout layout,
                           VkPipelineStageFlags2 &stage,
                           VkAccessFlags2 &access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        break;
    default:
        // UNDEFINED and anything else: nothing to wait on, nothing to flush.
        stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        access = 0;
        break;
    }
}

void VulkanGBuffer::transitionColor(VkCommandBuffer cmd, VkImageLayout to) {
    if (_colorLayout == to) {
        return;
    }
    VkPipelineStageFlags2 srcStage, dstStage;
    VkAccessFlags2 srcAccess, dstAccess;
    scopeForLayout(_colorLayout, srcStage, srcAccess);
    scopeForLayout(to, dstStage, dstAccess);

    std::array<VkImageMemoryBarrier2, Count> barriers {};
    for (int i = 0; i < Count; ++i) {
        auto &b = barriers[i];
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = _colorLayout;
        b.newLayout = to;
        b.image = _color[i]->handle();
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
    }
    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);
    _colorLayout = to;
}

void VulkanGBuffer::transitionDepth(VkCommandBuffer cmd, VkImageLayout to) {
    if (_depthLayout == to) {
        return;
    }
    VkPipelineStageFlags2 srcStage, dstStage;
    VkAccessFlags2 srcAccess, dstAccess;
    scopeForLayout(_depthLayout, srcStage, srcAccess);
    scopeForLayout(to, dstStage, dstAccess);

    VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = _depthLayout;
    b.newLayout = to;
    b.image = _depth->handle();
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;

    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    _depthLayout = to;
}

} // namespace graphics

} // namespace reone
