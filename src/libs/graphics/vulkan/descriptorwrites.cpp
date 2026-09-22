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

#include "reone/graphics/vulkan/descriptorwrites.h"

#include "reone/graphics/vulkan/rhi.h"

#include <stdexcept>

namespace reone::graphics {
namespace {

bool isImageDescriptor(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

bool isBufferDescriptor(VkDescriptorType type) {
    return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
           type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
           type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

VkWriteDescriptorSet makeWrite(VkDescriptorSet set, DescriptorBinding binding,
                               uint32_t arrayElement) {
    VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding.index;
    write.dstArrayElement = arrayElement;
    write.descriptorCount = 1;
    write.descriptorType = binding.type;
    return write;
}

} // namespace

void DescriptorWriteBuilder::writeImage(VkDescriptorSet set, DescriptorBinding binding,
                                        const VkDescriptorImageInfo &info, uint32_t arrayElement) {
    if (!isImageDescriptor(binding.type)) {
        throw std::invalid_argument("Vulkan: image descriptor write has a non-image type");
    }
    _imageInfos.push_back(info);
    auto write = makeWrite(set, binding, arrayElement);
    write.pImageInfo = &_imageInfos.back();
    _writes.push_back(write);
}

void DescriptorWriteBuilder::writeStorageImage(DescriptorSet set, DescriptorBinding binding,
                                                ImageView view, uint32_t arrayElement) {
    writeImage(toVulkanDescriptorSet(set), binding,
               {VK_NULL_HANDLE, toVulkanImageView(view), VK_IMAGE_LAYOUT_GENERAL}, arrayElement);
}

void DescriptorWriteBuilder::writeSampledImage(DescriptorSet set, DescriptorBinding binding,
                                                Sampler sampler, ImageView view,
                                                uint32_t arrayElement) {
    writeImage(toVulkanDescriptorSet(set), binding,
               {toVulkanSampler(sampler), toVulkanImageView(view),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, arrayElement);
}

void DescriptorWriteBuilder::writeBuffer(VkDescriptorSet set, DescriptorBinding binding,
                                         const VkDescriptorBufferInfo &info, uint32_t arrayElement) {
    if (!isBufferDescriptor(binding.type)) {
        throw std::invalid_argument("Vulkan: buffer descriptor write has a non-buffer type");
    }
    _bufferInfos.push_back(info);
    auto write = makeWrite(set, binding, arrayElement);
    write.pBufferInfo = &_bufferInfos.back();
    _writes.push_back(write);
}

void DescriptorWriteBuilder::writeAccelerationStructure(
    VkDescriptorSet set, DescriptorBinding binding,
    VkAccelerationStructureKHR accelerationStructure, uint32_t arrayElement) {
    if (binding.type != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
        throw std::invalid_argument("Vulkan: acceleration-structure descriptor write has the wrong type");
    }
    _accelerationStructureInfos.emplace_back();
    auto &stored = _accelerationStructureInfos.back();
    stored.handle = accelerationStructure;
    stored.info.accelerationStructureCount = 1;
    stored.info.pAccelerationStructures = &stored.handle;
    auto write = makeWrite(set, binding, arrayElement);
    write.pNext = &stored.info;
    _writes.push_back(write);
}

void DescriptorWriteBuilder::writeAccelerationStructure(
    VkDescriptorSet set, DescriptorBinding binding,
    TracingStructure structure, uint32_t arrayElement) {
    writeAccelerationStructure(set, binding, toVulkanTracingStructure(structure), arrayElement);
}

void DescriptorWriteBuilder::apply() const {
    if (_writes.empty()) {
        return;
    }
    vkUpdateDescriptorSets(_device, static_cast<uint32_t>(_writes.size()), _writes.data(), 0, nullptr);
}

} // namespace reone::graphics
