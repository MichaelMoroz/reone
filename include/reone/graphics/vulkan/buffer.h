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

#include <volk.h>

#include <vk_mem_alloc.h>

namespace reone {

namespace graphics {

class VulkanDevice;

/**
 * A buffer and the allocation backing it, freed together.
 *
 * Two kinds are worth distinguishing, and the difference is where the memory
 * lives rather than what the buffer is for: host-visible memory the CPU writes
 * every frame, and device-local memory written once through a staging copy.
 */
class VulkanBuffer : boost::noncopyable {
public:
    VulkanBuffer(VulkanDevice &device) :
        _device(device) {
    }

    ~VulkanBuffer() { deinit(); }

    /**
     * Memory the CPU can write directly and that stays mapped for the lifetime
     * of the buffer. For data that changes every frame: mapping and unmapping
     * repeatedly costs more than the residency does.
     */
    void initHostVisible(VkDeviceSize size, VkBufferUsageFlags usage);

    /**
     * Memory on the device, filled once from @p data through a staging buffer.
     * For vertices, indices and anything else written at load time.
     */
    void initDeviceLocal(VkDeviceSize size, VkBufferUsageFlags usage, const void *data);

    void deinit();

    VkBuffer handle() const { return _buffer; }
    VkDeviceSize size() const { return _size; }

    /**
     * GPU address for a buffer created with SHADER_DEVICE_ADDRESS usage, or
     * zero when this buffer is not addressable.
     */
    VkDeviceAddress deviceAddress() const;

    /** Null unless host-visible. */
    void *mapped() const { return _info.pMappedData; }

private:
    VulkanDevice &_device;

    VkBuffer _buffer {VK_NULL_HANDLE};
    VmaAllocation _allocation {VK_NULL_HANDLE};
    VmaAllocationInfo _info {};
    VkDeviceSize _size {0};
    bool _deviceAddressable {false};
};

} // namespace graphics

} // namespace reone
