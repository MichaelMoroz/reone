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

#include "reone/graphics/vulkan/descriptors.h"

#include "reone/graphics/types.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/uniformring.h"

namespace reone {

namespace graphics {

// The layout is generated from the binding points rather than written out, so
// there is one place to change and no chance of the two drifting apart.
static_assert(TextureUnits::gBufMotion == VulkanDescriptors::kNumTextures - 1,
              "kNumTextures must cover every unit in TextureUnits");

static_assert(UniformBlockBindingPoints::screenEffect ==
                  VulkanDescriptors::kNumUniformBlocks - 1,
              "kNumUniformBlocks must cover every binding point in uniforms.h");

/**
 * The size of the block at each binding, in binding order.
 *
 * A dynamic uniform descriptor needs a real range rather than VK_WHOLE_SIZE:
 * the range is what the shader may read starting at the dynamic offset, so
 * VK_WHOLE_SIZE would let it read from its slice to the end of the arena - into
 * other draws' data - and validation could not tell that was wrong.
 */
static constexpr VkDeviceSize kBlockSizes[VulkanDescriptors::kNumUniformBlocks] {
    sizeof(GlobalUniforms),      // 0 globals
    sizeof(LocalUniforms),       // 1 locals
    sizeof(BoneUniforms),        // 2 bones
    sizeof(DanglyUniforms),      // 3 dangly
    sizeof(AABBUniforms),        // 4 aabb
    sizeof(ParticleUniforms),    // 5 particles
    sizeof(GrassUniforms),       // 6 grass
    sizeof(WalkmeshUniforms),    // 7 walkmesh
    sizeof(TextUniforms),        // 8 text
    sizeof(ScreenEffectUniforms) // 9 screenEffect
};

VulkanDescriptors::~VulkanDescriptors() {
    deinit();
}

void VulkanDescriptors::init(int framesInFlight, VulkanUniformRing &ring) {
    std::array<VkDescriptorSetLayoutBinding, kNumUniformBlocks> bindings {};
    for (int i = 0; i < kNumUniformBlocks; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[i].descriptorCount = 1;
        // Every block is visible to every stage. The blocks are small, the
        // alternative is tracking which shader reads which, and Slang already
        // declares them all in one module shared by all stages.
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(_device.handle(), &layoutInfo, nullptr, &_uniformLayout) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: descriptor set layout creation failed");
    }

    std::array<VkDescriptorSetLayoutBinding, kNumTextures> textureBindings {};
    for (int i = 0; i < kNumTextures; ++i) {
        textureBindings[i].binding = i;
        textureBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        textureBindings[i].descriptorCount = 1;
        // Fragment only for now. Vertex-stage sampling exists in the shadow and
        // displacement paths and can be widened when those arrive.
        textureBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo textureLayoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    textureLayoutInfo.bindingCount = static_cast<uint32_t>(textureBindings.size());
    textureLayoutInfo.pBindings = textureBindings.data();
    if (vkCreateDescriptorSetLayout(_device.handle(), &textureLayoutInfo, nullptr, &_textureLayout) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: texture descriptor set layout creation failed");
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes {};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[0].descriptorCount = kNumUniformBlocks * framesInFlight;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kNumTextures;

    VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = static_cast<uint32_t>(framesInFlight) + 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (vkCreateDescriptorPool(_device.handle(), &poolInfo, nullptr, &_pool) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: descriptor pool creation failed");
    }

    std::vector<VkDescriptorSetLayout> layouts(framesInFlight, _uniformLayout);
    VkDescriptorSetAllocateInfo allocInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = _pool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocInfo.pSetLayouts = layouts.data();
    _uniformSets.resize(framesInFlight);
    if (vkAllocateDescriptorSets(_device.handle(), &allocInfo, _uniformSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: descriptor set allocation failed");
    }

    // Written once. Every binding covers that frame's whole arena; which slice a
    // draw reads is chosen by the dynamic offset passed at bind time.
    for (int frame = 0; frame < framesInFlight; ++frame) {
        std::array<VkDescriptorBufferInfo, kNumUniformBlocks> bufferInfos {};
        std::array<VkWriteDescriptorSet, kNumUniformBlocks> writes {};
        for (int i = 0; i < kNumUniformBlocks; ++i) {
            bufferInfos[i].buffer = ring.buffer(frame);
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = kBlockSizes[i];

            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = _uniformSets[frame];
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[i].pBufferInfo = &bufferInfos[i];
        }
        vkUpdateDescriptorSets(_device.handle(),
                               static_cast<uint32_t>(writes.size()), writes.data(),
                               0, nullptr);
    }

    VkSamplerCreateInfo samplerInfo {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(_device.handle(), &samplerInfo, nullptr, &_sampler) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: sampler creation failed");
    }

    VkDescriptorSetAllocateInfo textureAllocInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    textureAllocInfo.descriptorPool = _pool;
    textureAllocInfo.descriptorSetCount = 1;
    textureAllocInfo.pSetLayouts = &_textureLayout;
    if (vkAllocateDescriptorSets(_device.handle(), &textureAllocInfo, &_textureSet) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: texture descriptor set allocation failed");
    }

    const uint32_t white = 0xffffffff;
    _defaultTexture = std::make_unique<VulkanImage>(_device);
    _defaultTexture->initSampled2D({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, &white);
    for (int i = 0; i < kNumTextures; ++i) {
        setTexture(i, *_defaultTexture);
    }
}

void VulkanDescriptors::setTexture(int unit, const VulkanImage &image) {
    VkDescriptorImageInfo imageInfo {};
    imageInfo.sampler = _sampler;
    imageInfo.imageView = image.view();
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = _textureSet;
    write.dstBinding = static_cast<uint32_t>(unit);
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(_device.handle(), 1, &write, 0, nullptr);
}

void VulkanDescriptors::deinit() {
    _defaultTexture.reset();
    if (_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(_device.handle(), _sampler, nullptr);
        _sampler = VK_NULL_HANDLE;
    }
    if (_textureLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.handle(), _textureLayout, nullptr);
        _textureLayout = VK_NULL_HANDLE;
    }
    if (_pool != VK_NULL_HANDLE) {
        // Frees the sets with it.
        vkDestroyDescriptorPool(_device.handle(), _pool, nullptr);
        _pool = VK_NULL_HANDLE;
        _uniformSets.clear();
        _textureSet = VK_NULL_HANDLE;
    }
    if (_uniformLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.handle(), _uniformLayout, nullptr);
        _uniformLayout = VK_NULL_HANDLE;
    }
}

} // namespace graphics

} // namespace reone
