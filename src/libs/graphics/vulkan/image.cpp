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

#include "reone/graphics/vulkan/image.h"

#include "reone/graphics/types.h"
#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/device.h"

namespace reone {

namespace graphics {

static VkDeviceSize texelSize(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
        return 4;
    case VK_FORMAT_R8_UNORM:
        return 1;
    case VK_FORMAT_R16_SFLOAT:
        return 2;
    case VK_FORMAT_R16G16_SFLOAT:
        return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    case VK_FORMAT_D32_SFLOAT:
        return 4;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return 4;
    default:
        throw std::invalid_argument("Vulkan: unsupported image format");
    }
}

void VulkanImage::initSampled2D(glm::ivec2 extent, VkFormat format, const void *data) {
    VkDeviceSize size = data
                            ? static_cast<VkDeviceSize>(extent.x) * extent.y * texelSize(format)
                            : 0;
    initSampled2DSized(extent, format, data, size);
}

void VulkanImage::initSampled2DSized(glm::ivec2 extent,
                                     VkFormat format,
                                     const void *data,
                                     VkDeviceSize size) {
    _extent = extent;
    _format = format;

    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(_device.allocator(), &imageInfo, &allocInfo,
                       &_image, &_allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: image allocation failed");
    }

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &_view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: image view creation failed");
    }

    if (!data) {
        return;
    }

    VulkanBuffer staging(_device);
    staging.initHostVisible(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped(), data, static_cast<size_t>(size));

    auto image = _image;
    auto src = staging.handle();
    _device.immediateSubmit([image, src, extent](VkCommandBuffer cmd) {
        auto barrier = [&](VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                           VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
            VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = srcStage;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = dstStage;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.image = image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;

            VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
        vkCmdCopyBufferToImage(cmd, src, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
}

void VulkanImage::initSampledLayered(glm::ivec2 extent,
                                     VkFormat format,
                                     int layers,
                                     bool cube,
                                     const void *data) {
    _extent = extent;
    _format = format;

    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = static_cast<uint32_t>(layers);
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (cube) {
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(_device.allocator(), &imageInfo, &allocInfo,
                       &_image, &_allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: layered image allocation failed");
    }

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = static_cast<uint32_t>(layers);
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &_view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: layered image view creation failed");
    }

    if (!data) {
        return;
    }

    VkDeviceSize perLayer = static_cast<VkDeviceSize>(extent.x) * extent.y * texelSize(format);
    std::vector<uint8_t> repeated(static_cast<size_t>(perLayer) * layers);
    for (int i = 0; i < layers; ++i) {
        std::memcpy(repeated.data() + i * perLayer, data, static_cast<size_t>(perLayer));
    }

    VulkanBuffer staging(_device);
    staging.initHostVisible(repeated.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped(), repeated.data(), repeated.size());

    auto image = _image;
    auto src = staging.handle();
    _device.immediateSubmit([image, src, extent, layers](VkCommandBuffer cmd) {
        auto barrier = [&](VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                           VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
            VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = srcStage;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = dstStage;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.image = image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = static_cast<uint32_t>(layers);

            VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = static_cast<uint32_t>(layers);
        region.imageExtent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
        vkCmdCopyBufferToImage(cmd, src, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
}

void VulkanImage::initSampledLayers(
    glm::ivec2 extent,
    VkFormat format,
    bool cube,
    const std::vector<std::pair<const void *, VkDeviceSize>> &layers) {
    if (layers.empty()) {
        throw std::invalid_argument("Vulkan: a layered image needs at least one layer");
    }
    _extent = extent;
    _format = format;

    auto layerCount = static_cast<uint32_t>(layers.size());

    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layerCount;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (cube) {
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(_device.allocator(), &imageInfo, &allocInfo,
                       &_image, &_allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: layered image allocation failed");
    }

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = layerCount;
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &_view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: layered image view creation failed");
    }

    // One staging buffer for the lot, with a copy region per layer pointing at
    // its own offset. Layers are packed back to back at their natural size,
    // which for a block-compressed face is not width * height * texel size.
    VkDeviceSize total = 0;
    std::vector<VkDeviceSize> offsets;
    offsets.reserve(layers.size());
    for (const auto &layer : layers) {
        offsets.push_back(total);
        total += layer.second;
    }
    if (total == 0) {
        return;
    }

    VulkanBuffer staging(_device);
    staging.initHostVisible(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto *mapped = static_cast<uint8_t *>(staging.mapped());
    for (size_t i = 0; i < layers.size(); ++i) {
        if (layers[i].first && layers[i].second > 0) {
            std::memcpy(mapped + offsets[i], layers[i].first,
                        static_cast<size_t>(layers[i].second));
        }
    }

    std::vector<VkBufferImageCopy> regions;
    regions.reserve(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        if (!layers[i].first || layers[i].second == 0) {
            continue;
        }
        VkBufferImageCopy region {};
        region.bufferOffset = offsets[i];
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.baseArrayLayer = static_cast<uint32_t>(i);
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(extent.x),
                              static_cast<uint32_t>(extent.y), 1};
        regions.push_back(region);
    }

    auto image = _image;
    auto src = staging.handle();
    _device.immediateSubmit([image, src, layerCount, &regions](VkCommandBuffer cmd) {
        auto barrier = [&](VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                           VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
            VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = srcStage;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = dstStage;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.image = image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = layerCount;

            VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (!regions.empty()) {
            vkCmdCopyBufferToImage(cmd, src, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<uint32_t>(regions.size()), regions.data());
        }
        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
}

void VulkanImage::initSampledChain(glm::ivec2 extent,
                                   VkFormat format,
                                   bool cube,
                                   uint32_t layerCount,
                                   uint32_t mipCount,
                                   const std::vector<Subresource> &subresources,
                                   bool generateMips) {
    if (layerCount == 0 || mipCount == 0) {
        throw std::invalid_argument("Vulkan: an image needs at least one layer and one mip");
    }
    _extent = extent;
    _format = format;
    _mipLevels = static_cast<int>(mipCount);

    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
    imageInfo.mipLevels = mipCount;
    imageInfo.arrayLayers = layerCount;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (generateMips) {
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (cube) {
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(_device.allocator(), &imageInfo, &allocInfo,
                       &_image, &_allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: image allocation failed");
    }

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE
                             : (layerCount > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                               : VK_IMAGE_VIEW_TYPE_2D);
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipCount;
    viewInfo.subresourceRange.layerCount = layerCount;
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &_view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: image view creation failed");
    }

    // One staging buffer for every subresource, packed back to back at each
    // one's own length - which for a compressed level is a block count, not a
    // texel count, so it cannot be derived from the extent here.
    VkDeviceSize total = 0;
    std::vector<VkDeviceSize> offsets;
    offsets.reserve(subresources.size());
    for (const auto &sub : subresources) {
        offsets.push_back(total);
        total += sub.size;
    }
    if (total == 0) {
        return;
    }

    VulkanBuffer staging(_device);
    staging.initHostVisible(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto *mapped = static_cast<uint8_t *>(staging.mapped());
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(subresources.size());
    for (size_t i = 0; i < subresources.size(); ++i) {
        const auto &sub = subresources[i];
        if (!sub.data || sub.size == 0) {
            continue;
        }
        std::memcpy(mapped + offsets[i], sub.data, static_cast<size_t>(sub.size));

        VkBufferImageCopy region {};
        region.bufferOffset = offsets[i];
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = sub.mip;
        region.imageSubresource.baseArrayLayer = sub.layer;
        region.imageSubresource.layerCount = 1;
        // The extent is the level's logical size even below one block, where
        // the byte count still covers a whole block.
        region.imageExtent = {
            std::max(1u, static_cast<uint32_t>(extent.x) >> sub.mip),
            std::max(1u, static_cast<uint32_t>(extent.y) >> sub.mip),
            1};
        regions.push_back(region);
    }

    auto image = _image;
    auto src = staging.handle();
    _device.immediateSubmit([image, src, extent, layerCount, mipCount, generateMips,
                             &regions](VkCommandBuffer cmd) {
        auto barrier = [&](VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                           VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                           uint32_t baseMip, uint32_t levelCount) {
            VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = srcStage;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = dstStage;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.image = image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.baseMipLevel = baseMip;
            b.subresourceRange.levelCount = levelCount;
            b.subresourceRange.layerCount = layerCount;

            VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, 0, mipCount);
        if (!regions.empty()) {
            vkCmdCopyBufferToImage(cmd, src, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   static_cast<uint32_t>(regions.size()), regions.data());
        }
        if (!generateMips) {
            barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    0, mipCount);
            return;
        }

        for (uint32_t mip = 0; mip + 1 < mipCount; ++mip) {
            barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, mip, 1);

            VkImageBlit blit {};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = mip;
            blit.srcSubresource.layerCount = layerCount;
            blit.srcOffsets[1] = {std::max(1, extent.x >> static_cast<int>(mip)),
                                  std::max(1, extent.y >> static_cast<int>(mip)), 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = mip + 1;
            blit.dstSubresource.layerCount = layerCount;
            blit.dstOffsets[1] = {std::max(1, extent.x >> static_cast<int>(mip + 1)),
                                  std::max(1, extent.y >> static_cast<int>(mip + 1)), 1};
            vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

            barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    mip, 1);
        }
        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                mipCount - 1, 1);
    });
}

/** The bytes a tightly packed image copy occupies, including BC blocks. */
static VkDeviceSize imageSize(VkFormat format, glm::ivec2 extent, uint32_t layers) {
    switch (format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        return static_cast<VkDeviceSize>((extent.x + 3) / 4) * ((extent.y + 3) / 4) * layers * 8;
    case VK_FORMAT_BC3_UNORM_BLOCK:
        return static_cast<VkDeviceSize>((extent.x + 3) / 4) * ((extent.y + 3) / 4) * layers * 16;
    default:
        return static_cast<VkDeviceSize>(extent.x) * extent.y * layers * texelSize(format);
    }
}

void VulkanImage::initDepthLayered(glm::ivec2 extent, VkFormat format, int layers, bool cube) {
    _extent = extent;
    _format = format;

    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = static_cast<uint32_t>(layers);
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (cube) {
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(_device.allocator(), &imageInfo, &allocInfo,
                       &_image, &_allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: layered depth image allocation failed");
    }

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = static_cast<uint32_t>(layers);
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &_view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: layered depth image view creation failed");
    }
}

void VulkanImage::initSampledCubeArray(glm::ivec2 faceExtent, VkFormat format,
                                       int cubes, const void *data) {
    _extent = faceExtent;
    _format = format;
    _mipLevels = 1;

    const uint32_t layers = static_cast<uint32_t>(cubes * kNumCubeFaces);

    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {static_cast<uint32_t>(faceExtent.x),
                        static_cast<uint32_t>(faceExtent.y), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(_device.allocator(), &imageInfo, &allocInfo,
                       &_image, &_allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: cube array image allocation failed");
    }

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = layers;
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &_view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: cube array image view creation failed");
    }

    if (!data) {
        return;
    }
    VkDeviceSize perLayer = static_cast<VkDeviceSize>(faceExtent.x) * faceExtent.y *
                            texelSize(format);
    std::vector<uint8_t> repeated(static_cast<size_t>(perLayer) * layers);
    for (uint32_t i = 0; i < layers; ++i) {
        std::memcpy(repeated.data() + i * perLayer, data, static_cast<size_t>(perLayer));
    }

    VulkanBuffer staging(_device);
    staging.initHostVisible(repeated.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped(), repeated.data(), repeated.size());

    auto image = _image;
    auto src = staging.handle();
    _device.immediateSubmit([image, src, faceExtent, layers](VkCommandBuffer cmd) {
        auto barrier = [&](VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                           VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
            VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = srcStage;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = dstStage;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.image = image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = layers;

            VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };
        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);

        VkBufferImageCopy region {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = layers;
        region.imageExtent = {static_cast<uint32_t>(faceExtent.x),
                              static_cast<uint32_t>(faceExtent.y), 1};
        vkCmdCopyBufferToImage(cmd, src, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &region);

        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
}

void VulkanImage::initCubeArrayAttachment(glm::ivec2 faceExtent, VkFormat format,
                                          int cubes, int mips) {
    _extent = faceExtent;
    _format = format;
    _mipLevels = mips;

    const uint32_t layers = static_cast<uint32_t>(cubes * kNumCubeFaces);

    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {static_cast<uint32_t>(faceExtent.x),
                        static_cast<uint32_t>(faceExtent.y), 1};
    imageInfo.mipLevels = static_cast<uint32_t>(mips);
    imageInfo.arrayLayers = layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(_device.allocator(), &imageInfo, &allocInfo,
                       &_image, &_allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: cube array image allocation failed");
    }

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = static_cast<uint32_t>(mips);
    viewInfo.subresourceRange.layerCount = layers;
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &_view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: cube array image view creation failed");
    }
}

VkImageView VulkanImage::renderView(int cube, int mip) {
    int key = cube * _mipLevels + mip;
    auto existing = _renderViews.find(key);
    if (existing != _renderViews.end()) {
        return existing->second;
    }
    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    // A 2D array of exactly the six faces, which is what a six-view render pass
    // writes into. A cube view cannot be a colour attachment.
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = _format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = static_cast<uint32_t>(mip);
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = static_cast<uint32_t>(cube * kNumCubeFaces);
    viewInfo.subresourceRange.layerCount = kNumCubeFaces;

    VkImageView view {VK_NULL_HANDLE};
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: cube array render view creation failed");
    }
    _renderViews.insert({key, view});
    return view;
}

VkImageView VulkanImage::cubeView(int cube) {
    auto existing = _cubeViews.find(cube);
    if (existing != _cubeViews.end()) {
        return existing->second;
    }
    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = _format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = static_cast<uint32_t>(_mipLevels);
    viewInfo.subresourceRange.baseArrayLayer = static_cast<uint32_t>(cube * kNumCubeFaces);
    viewInfo.subresourceRange.layerCount = kNumCubeFaces;
    VkImageView view {VK_NULL_HANDLE};
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: cube image view creation failed");
    }
    _cubeViews.insert({cube, view});
    return view;
}

VkImageView VulkanImage::faceRenderView(int cube, int face, int mip) {
    int key = (cube * _mipLevels + mip) * kNumCubeFaces + face;
    auto existing = _faceRenderViews.find(key);
    if (existing != _faceRenderViews.end()) {
        return existing->second;
    }
    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = _format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = static_cast<uint32_t>(mip);
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = static_cast<uint32_t>(cube * kNumCubeFaces + face);
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView view {VK_NULL_HANDLE};
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: cube-face render view creation failed");
    }
    _faceRenderViews.insert({key, view});
    return view;
}

void VulkanImage::initColorAttachment(glm::ivec2 extent, VkFormat format) {
    _extent = extent;
    _format = format;

    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC so the target can be read back for a G-buffer dump. Not
    // free, but a colour attachment that cannot be copied out of cannot be
    // compared against the other backend either. TRANSFER_DST because the
    // filter chain copies one of these back over another when it runs an odd
    // number of passes.
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(_device.allocator(), &imageInfo, &allocInfo,
                       &_image, &_allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: colour attachment allocation failed");
    }

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &_view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: colour attachment view creation failed");
    }
}

void VulkanImage::initDepth(glm::ivec2 extent, VkFormat format) {
    _extent = extent;
    _format = format;

    VkImageCreateInfo imageInfo {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // Sampled as well as written: the deferred resolve reconstructs world
    // position from depth, so the same image is read back as a texture.
    // TRANSFER_SRC additionally lets it be dumped for comparison.
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(_device.allocator(), &imageInfo, &allocInfo,
                       &_image, &_allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: depth image allocation failed");
    }

    VkImageViewCreateInfo viewInfo {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = _image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(_device.handle(), &viewInfo, nullptr, &_view) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: depth image view creation failed");
    }
}

std::vector<uint8_t> VulkanImage::readBack(VkImageLayout layout, bool depth) const {
    auto aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkDeviceSize size = imageSize(_format, _extent, 1);

    VulkanBuffer staging(_device);
    staging.initHostVisible(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    auto image = _image;
    auto dst = staging.handle();
    auto extent = _extent;
    _device.immediateSubmit([image, dst, extent, aspect, layout](VkCommandBuffer cmd) {
        auto barrier = [&](VkImageLayout from, VkImageLayout to) {
            VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            // Conservative on both sides: this runs once, outside the frame, and
            // getting a dump slightly wrong is worse than getting it slowly.
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = from;
            b.newLayout = to;
            b.image = image;
            b.subresourceRange.aspectMask = aspect;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = 1;

            VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        barrier(layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

        VkBufferImageCopy region {};
        region.imageSubresource.aspectMask = aspect;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(extent.x),
                              static_cast<uint32_t>(extent.y), 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst, 1, &region);

        // Put it back, so the next frame finds the image where it left it.
        barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
    });

    std::vector<uint8_t> result(static_cast<size_t>(size));
    std::memcpy(result.data(), staging.mapped(), result.size());
    return result;
}

std::vector<uint8_t> VulkanImage::readBack(VkImageLayout layout, uint32_t mip,
                                           uint32_t layers) const {
    auto extent = glm::max(glm::ivec2(1), _extent >> static_cast<int>(mip));
    VkDeviceSize size = imageSize(_format, extent, layers);

    VulkanBuffer staging(_device);
    staging.initHostVisible(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    auto image = _image;
    auto dst = staging.handle();
    _device.immediateSubmit([image, dst, extent, mip, layers, layout](VkCommandBuffer cmd) {
        auto barrier = [&](VkImageLayout from, VkImageLayout to) {
            VkImageMemoryBarrier2 b {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = from;
            b.newLayout = to;
            b.image = image;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.baseMipLevel = mip;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = layers;

            VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
        };

        barrier(layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = mip;
        region.imageSubresource.layerCount = layers;
        region.imageExtent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
        barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
    });

    std::vector<uint8_t> result(static_cast<size_t>(size));
    std::memcpy(result.data(), staging.mapped(), result.size());
    return result;
}

void VulkanImage::deinit() {
    for (auto &[key, view] : _renderViews) {
        vkDestroyImageView(_device.handle(), view, nullptr);
    }
    _renderViews.clear();
    for (auto &[key, view] : _cubeViews) {
        vkDestroyImageView(_device.handle(), view, nullptr);
    }
    _cubeViews.clear();
    for (auto &[key, view] : _faceRenderViews) {
        vkDestroyImageView(_device.handle(), view, nullptr);
    }
    _faceRenderViews.clear();
    if (_view != VK_NULL_HANDLE) {
        vkDestroyImageView(_device.handle(), _view, nullptr);
        _view = VK_NULL_HANDLE;
    }
    if (_image != VK_NULL_HANDLE) {
        vmaDestroyImage(_device.allocator(), _image, _allocation);
        _image = VK_NULL_HANDLE;
        _allocation = VK_NULL_HANDLE;
    }
}

} // namespace graphics

} // namespace reone
