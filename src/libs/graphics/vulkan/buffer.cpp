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

VulkanBuffer &toVulkanBuffer(IBuffer &buffer) {
    auto *result = dynamic_cast<VulkanBuffer *>(&buffer);
    if (!result) {
        throw std::invalid_argument("Buffer is not implemented by Vulkan");
    }
    return *result;
}

const VulkanBuffer &toVulkanBuffer(const IBuffer &buffer) {
    return toVulkanBuffer(const_cast<IBuffer &>(buffer));
}

void VulkanBuffer::initHostVisibleStorage(uint64_t size) {
    initHostVisible(static_cast<VkDeviceSize>(size), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
}

void VulkanBuffer::initDeviceStorage(uint64_t size, const void *data) {
    initDeviceLocal(static_cast<VkDeviceSize>(size), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, data);
}

void VulkanBuffer::initMergedGeometry(uint64_t size) {
    initDeviceLocal(static_cast<VkDeviceSize>(size),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    nullptr);
}

void VulkanBuffer::initHostVisibleReadback(uint64_t size) {
    initHostVisibleReadback(static_cast<VkDeviceSize>(size), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
}

void VulkanBuffer::uploadDeviceStorage(uint64_t offset, uint64_t size, const void *data) {
    uploadDeviceLocal(static_cast<VkDeviceSize>(offset), static_cast<VkDeviceSize>(size), data);
}

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
    _deviceAddressable = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
}

void VulkanBuffer::initHostVisibleReadback(VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = size;
    bufInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (vmaCreateBuffer(_device.allocator(), &bufInfo, &allocInfo,
                        &_buffer, &_allocation, &_info) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: host-visible readback buffer allocation failed");
    }
    _size = size;
    _deviceAddressable = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
}

void VulkanBuffer::invalidateMapped() const {
    if (_allocation != VK_NULL_HANDLE) {
        vmaInvalidateAllocation(_device.allocator(), _allocation, 0, VK_WHOLE_SIZE);
    }
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
    _deviceAddressable = (bufInfo.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;

    if (!data) {
        return;
    }

    uploadDeviceLocal(0, size, data);
}

void VulkanBuffer::uploadDeviceLocal(VkDeviceSize offset, VkDeviceSize size, const void *data) {
    if (_buffer == VK_NULL_HANDLE || !data || size == 0 || offset + size > _size) {
        throw std::invalid_argument("Vulkan: invalid device-local buffer upload");
    }
    VulkanBuffer staging(_device);
    staging.initHostVisible(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped(), data, static_cast<size_t>(size));

    auto src = staging.handle();
    auto dst = _buffer;
    _device.immediateSubmit([src, dst, offset, size](VkCommandBuffer cmd) {
        VkBufferCopy copy {};
        copy.srcOffset = 0;
        copy.dstOffset = offset;
        copy.size = size;
        vkCmdCopyBuffer(cmd, src, dst, 1, &copy);
    });
}

VkDeviceAddress VulkanBuffer::deviceAddress() const {
    if (!_deviceAddressable || _buffer == VK_NULL_HANDLE) {
        return 0;
    }

    VkBufferDeviceAddressInfo info {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = _buffer;
    return vkGetBufferDeviceAddress(_device.handle(), &info);
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
    _deviceAddressable = false;
}

} // namespace graphics

} // namespace reone
