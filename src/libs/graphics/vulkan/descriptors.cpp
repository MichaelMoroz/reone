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

/**
 * The layout an image is sampled from.
 *
 * A depth image cannot sit in SHADER_READ_ONLY_OPTIMAL while it is also serving
 * as a read-only depth attachment, so it lives in DEPTH_READ_ONLY_OPTIMAL and
 * the descriptor has to say so. Inferred from the format rather than asked of
 * the caller, because the caller has no better way to know than this does.
 */
static VkImageLayout sampledLayoutFor(const VulkanImage &image) {
    switch (image.format()) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
        return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    default:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}


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

    VkDescriptorPoolSize poolSize {};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSize.descriptorCount = kNumUniformBlocks * framesInFlight;

    VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = static_cast<uint32_t>(framesInFlight);
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
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

    const uint32_t white = 0xffffffff;
    _default2D = std::make_unique<VulkanImage>(_device);
    _default2D->initSampled2D({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, &white);
    _defaultArray = std::make_unique<VulkanImage>(_device);
    _defaultArray->initSampledLayered({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, 1, false, &white);
    _defaultCube = std::make_unique<VulkanImage>(_device);
    _defaultCube->initSampledLayered({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, 6, true, &white);
    // Black: this stands in for incoming radiance, and a white one would light
    // every environment-mapped surface from all directions at full strength.
    const uint32_t black = 0xff000000;
    _defaultCubeArray = std::make_unique<VulkanImage>(_device);
    _defaultCubeArray->initSampledCubeArray({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, 1, &black);
    for (int i = 0; i < kNumTextures; ++i) {
        _standing[i] = defaultFor(i, _default2D.get(), _defaultArray.get(), _defaultCube.get(),
                                  _defaultCubeArray.get());
    }

    // One pool per frame in flight, reset wholesale rather than freeing sets
    // individually. kMaxTextureSetsPerFrame caps how many distinct textures one
    // frame may draw with; exceeding it throws rather than corrupting.
    _textureFrames.resize(framesInFlight);
    for (auto &frame : _textureFrames) {
        VkDescriptorPoolSize size {};
        size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        size.descriptorCount = kNumTextures * kMaxTextureSetsPerFrame;

        VkDescriptorPoolCreateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = kMaxTextureSetsPerFrame;
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;
        if (vkCreateDescriptorPool(_device.handle(), &info, nullptr, &frame.pool) != VK_SUCCESS) {
            throw std::runtime_error("Vulkan: texture descriptor pool creation failed");
        }
    }
}

void VulkanDescriptors::setTexture(int unit, const VulkanImage &image) {
    _standing[unit] = &image;
}

void VulkanDescriptors::beginFrame(int frame) {
    auto &f = _textureFrames[frame];
    // Safe because the caller has already waited on this frame's fence.
    vkResetDescriptorPool(_device.handle(), f.pool, 0);
    f.byTexture.clear();
    f.byBindings.clear();
}

void VulkanDescriptors::writeTextureSet(VkDescriptorSet set, const VulkanImage *mainTex) {
    std::array<VkDescriptorImageInfo, kNumTextures> infos {};
    std::array<VkWriteDescriptorSet, kNumTextures> writes {};
    for (int i = 0; i < kNumTextures; ++i) {
        auto image = (i == TextureUnits::mainTex && mainTex) ? mainTex : _standing[i];
        infos[i].sampler = image->sampler() ? image->sampler() : _sampler;
        infos[i].imageView = image->view();
        infos[i].imageLayout = sampledLayoutFor(*image);

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(_device.handle(),
                           static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

VkDescriptorSet VulkanDescriptors::createPersistentTextureSet(
    const std::vector<std::pair<int, const VulkanImage *>> &bindings) {
    if (_persistentPool == VK_NULL_HANDLE) {
        VkDescriptorPoolSize size {};
        size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        size.descriptorCount = kNumTextures * kMaxPersistentTextureSets;

        VkDescriptorPoolCreateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = kMaxPersistentTextureSets;
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;
        if (vkCreateDescriptorPool(_device.handle(), &info, nullptr, &_persistentPool) != VK_SUCCESS) {
            throw std::runtime_error("Vulkan: persistent texture pool creation failed");
        }
    }

    VkDescriptorSetAllocateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = _persistentPool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &_textureLayout;
    VkDescriptorSet set {VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(_device.handle(), &info, &set) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: persistent texture set allocation failed");
    }

    std::array<VkDescriptorImageInfo, kNumTextures> infos {};
    std::array<VkWriteDescriptorSet, kNumTextures> writes {};
    for (int i = 0; i < kNumTextures; ++i) {
        auto image = defaultFor(i, _default2D.get(), _defaultArray.get(), _defaultCube.get(),
                                  _defaultCubeArray.get());
        for (const auto &[unit, override] : bindings) {
            if (unit == i) {
                image = override;
            }
        }
        infos[i].sampler = image->sampler() ? image->sampler() : _sampler;
        infos[i].imageView = image->view();
        infos[i].imageLayout = sampledLayoutFor(*image);

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(_device.handle(),
                           static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return set;
}

void VulkanDescriptors::writeTextureSet(
    VkDescriptorSet set,
    const std::vector<std::pair<int, const VulkanImage *>> &bindings) {
    std::array<VkDescriptorImageInfo, kNumTextures> infos {};
    std::array<VkWriteDescriptorSet, kNumTextures> writes {};
    for (int i = 0; i < kNumTextures; ++i) {
        auto image = _standing[i];
        for (const auto &binding : bindings) {
            if (binding.first == i && binding.second) {
                image = binding.second;
            }
        }
        infos[i].sampler = image->sampler() ? image->sampler() : _sampler;
        infos[i].imageView = image->view();
        infos[i].imageLayout = sampledLayoutFor(*image);

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(_device.handle(),
                           static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

VkDescriptorSet VulkanDescriptors::acquireTextureSet(
    int frame,
    const std::vector<std::pair<int, const VulkanImage *>> &bindings) {
    size_t key = bindings.size();
    for (const auto &binding : bindings) {
        key ^= (std::hash<const void *> {}(binding.second) ^
                static_cast<size_t>(binding.first) * 0x9e3779b9u) +
               (key << 6) + (key >> 2);
    }

    auto &f = _textureFrames[frame];
    auto existing = f.byBindings.find(key);
    if (existing != f.byBindings.end()) {
        return existing->second;
    }
    if (f.byTexture.size() + f.byBindings.size() >= kMaxTextureSetsPerFrame) {
        throw std::runtime_error("Vulkan: too many distinct texture sets in one frame");
    }

    VkDescriptorSetAllocateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = f.pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &_textureLayout;
    VkDescriptorSet set {VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(_device.handle(), &info, &set) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: texture descriptor set allocation failed");
    }
    writeTextureSet(set, bindings);
    f.byBindings.insert({key, set});
    return set;
}

VkDescriptorSet VulkanDescriptors::acquireTextureSet(int frame, const VulkanImage *mainTex) {
    auto &f = _textureFrames[frame];
    auto key = mainTex ? mainTex : _default2D.get();
    auto existing = f.byTexture.find(key);
    if (existing != f.byTexture.end()) {
        return existing->second;
    }
    if (f.byTexture.size() >= kMaxTextureSetsPerFrame) {
        throw std::runtime_error("Vulkan: too many distinct textures in one frame");
    }

    VkDescriptorSetAllocateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = f.pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &_textureLayout;
    VkDescriptorSet set {VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(_device.handle(), &info, &set) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: texture descriptor set allocation failed");
    }
    writeTextureSet(set, mainTex);
    f.byTexture.insert({key, set});
    return set;
}

const VulkanImage *VulkanDescriptors::defaultFor(int unit,
                                                 const VulkanImage *twoD,
                                                 const VulkanImage *array,
                                                 const VulkanImage *cube,
                                                 const VulkanImage *cubeArray) {
    switch (unit) {
    case TextureUnits::bumpMapArray:
    case TextureUnits::shadowMapArray:
        return array;
    case TextureUnits::irradianceMapArray:
    case TextureUnits::prefilteredEnvMapArray:
        return cubeArray;
    case TextureUnits::envMapCube:
    case TextureUnits::shadowMapCube:
        return cube;
    default:
        return twoD;
    }
}

void VulkanDescriptors::deinit() {
    if (_persistentPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(_device.handle(), _persistentPool, nullptr);
        _persistentPool = VK_NULL_HANDLE;
    }
    for (auto &frame : _textureFrames) {
        if (frame.pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(_device.handle(), frame.pool, nullptr);
        }
    }
    _textureFrames.clear();
    _default2D.reset();
    _defaultArray.reset();
    _defaultCube.reset();
    // Every default has to be released here rather than left to the member
    // destructor: this object outlives VulkanDevice::deinit, so an image still
    // holding a VMA allocation at that point is freed against an allocator that
    // no longer exists.
    _defaultCubeArray.reset();
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
    }
    if (_uniformLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.handle(), _uniformLayout, nullptr);
        _uniformLayout = VK_NULL_HANDLE;
    }
}

} // namespace graphics

} // namespace reone
