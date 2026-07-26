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

#include "reone/graphics/vulkan/buffer.h"

#include "reone/graphics/vulkan/device.h"

namespace reone {

namespace graphics {

void VulkanBuffer::initHostVisible(VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = size;
    bufInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    // SEQUENTIAL_WRITE, not RANDOM: this memory may be write-combined, where
    // reading back is enormously slow. Everything writing here writes forwards
    // and never reads.
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;

    if (vmaCreateBuffer(_device.allocator(), &bufInfo, &allocInfo,
                        &_buffer, &_allocation, &_info) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: host-visible buffer allocation failed");
    }
    _size = size;
}

void VulkanBuffer::initDeviceLocal(VkDeviceSize size, VkBufferUsageFlags usage, const void *data) {
    VkBufferCreateInfo bufInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = size;
    bufInfo.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateBuffer(_device.allocator(), &bufInfo, &allocInfo,
                        &_buffer, &_allocation, &_info) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: device-local buffer allocation failed");
    }
    _size = size;

    if (!data) {
        return;
    }

    VulkanBuffer staging(_device);
    staging.initHostVisible(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped(), data, static_cast<size_t>(size));

    auto src = staging.handle();
    auto dst = _buffer;
    _device.immediateSubmit([src, dst, size](VkCommandBuffer cmd) {
        VkBufferCopy copy {};
        copy.size = size;
        vkCmdCopyBuffer(cmd, src, dst, 1, &copy);
    });
}

void VulkanBuffer::deinit() {
    if (_buffer == VK_NULL_HANDLE) {
        return;
    }
    vmaDestroyBuffer(_device.allocator(), _buffer, _allocation);
    _buffer = VK_NULL_HANDLE;
    _allocation = VK_NULL_HANDLE;
    _info = {};
    _size = 0;
}

} // namespace graphics

} // namespace reone
