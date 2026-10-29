/*
 * Copyright (c) 2026 The reone project contributors
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

#include <deque>
#include <vector>

#include "reone/graphics/rhi/rhi.h"

namespace reone::graphics {

struct DescriptorBinding {
    uint32_t index;
    VkDescriptorType type;
};

/**
 * Batches descriptor writes while owning every Vulkan structure their raw
 * pointers reference. The backing deques keep those addresses stable as new
 * writes are appended.
 */
class DescriptorWriteBuilder {
public:
    explicit DescriptorWriteBuilder(VkDevice device) :
        _device(device) {
    }

    DescriptorWriteBuilder(const DescriptorWriteBuilder &) = delete;
    DescriptorWriteBuilder &operator=(const DescriptorWriteBuilder &) = delete;
    DescriptorWriteBuilder(DescriptorWriteBuilder &&) = delete;
    DescriptorWriteBuilder &operator=(DescriptorWriteBuilder &&) = delete;

    void writeImage(VkDescriptorSet set, DescriptorBinding binding,
                    const VkDescriptorImageInfo &info, uint32_t arrayElement = 0);
    void writeStorageImage(DescriptorSet set, DescriptorBinding binding, ImageView view,
                           uint32_t arrayElement = 0);
    void writeSampledImage(DescriptorSet set, DescriptorBinding binding, Sampler sampler,
                           ImageView view, uint32_t arrayElement = 0);
    void writeBuffer(VkDescriptorSet set, DescriptorBinding binding,
                     const VkDescriptorBufferInfo &info, uint32_t arrayElement = 0);
    void writeAccelerationStructure(VkDescriptorSet set, DescriptorBinding binding,
                                    VkAccelerationStructureKHR accelerationStructure,
                                    uint32_t arrayElement = 0);
    void writeAccelerationStructure(VkDescriptorSet set, DescriptorBinding binding,
                                    TracingStructure structure,
                                    uint32_t arrayElement = 0);

    void apply() const;

private:
    struct AccelerationStructureInfo {
        VkAccelerationStructureKHR handle {VK_NULL_HANDLE};
        VkWriteDescriptorSetAccelerationStructureKHR info {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    };

    VkDevice _device;
    std::deque<VkDescriptorImageInfo> _imageInfos;
    std::deque<VkDescriptorBufferInfo> _bufferInfos;
    std::deque<AccelerationStructureInfo> _accelerationStructureInfos;
    std::vector<VkWriteDescriptorSet> _writes;
};

} // namespace reone::graphics
