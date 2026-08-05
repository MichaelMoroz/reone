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

#include "reone/graphics/rhi/buffer.h"

#include "rhi.h"

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
class VulkanBuffer : public IBuffer, boost::noncopyable {
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

    /** Host-visible storage which the GPU writes and the CPU reads next frame. */
    void initHostVisibleReadback(VkDeviceSize size, VkBufferUsageFlags usage);

    /** Make GPU writes to a mapped readback allocation visible to the CPU. */
    void invalidateMapped() const override;

    /**
     * Memory on the device, filled once from @p data through a staging buffer.
     * For vertices, indices and anything else written at load time.
     */
    void initDeviceLocal(VkDeviceSize size, VkBufferUsageFlags usage, const void *data);

    /** Copy a byte range into an existing device-local buffer and wait for it. */
    void uploadDeviceLocal(VkDeviceSize offset, VkDeviceSize size, const void *data);
    void initHostVisibleStorage(uint64_t size) override;
    void initDeviceStorage(uint64_t size, const void *data) override;
    void initDeviceLocalStorage(uint64_t size) override;
    void initHostVisibleReadback(uint64_t size) override;
    void uploadDeviceStorage(uint64_t offset, uint64_t size, const void *data) override;

    void deinit() override;

    VkBuffer handle() const { return _buffer; }
    Buffer rhiHandle() const override { return toBuffer(_buffer); }
    uint64_t size() const override { return _size; }

    /**
     * GPU address for a buffer created with SHADER_DEVICE_ADDRESS usage, or
     * zero when this buffer is not addressable.
     */
    VkDeviceAddress deviceAddress() const;

    /** Null unless host-visible. */
    void *mapped() const override { return _info.pMappedData; }

private:
    VulkanDevice &_device;

    VkBuffer _buffer {VK_NULL_HANDLE};
    VmaAllocation _allocation {VK_NULL_HANDLE};
    VmaAllocationInfo _info {};
    VkDeviceSize _size {0};
    bool _deviceAddressable {false};
};

VulkanBuffer &toVulkanBuffer(IBuffer &buffer);
const VulkanBuffer &toVulkanBuffer(const IBuffer &buffer);

} // namespace graphics

} // namespace reone
