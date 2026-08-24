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
#include "reone/graphics/vulkan/descriptorwrites.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/resources.h"
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
static_assert(TextureUnits::coverage == VulkanDescriptors::kNumTextures - 1,
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
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
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
        // Fragment and compute. The PBR resolve reads this same table from a
        // dispatch, and one table serving both is the point of it; vertex-stage
        // sampling exists in the shadow and displacement paths and can be
        // added when those arrive.
        textureBindings[i].stageFlags =
            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo textureLayoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    textureLayoutInfo.bindingCount = static_cast<uint32_t>(textureBindings.size());
    textureLayoutInfo.pBindings = textureBindings.data();
    if (vkCreateDescriptorSetLayout(_device.handle(), &textureLayoutInfo, nullptr, &_textureLayout) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: texture descriptor set layout creation failed");
    }

    // The resolve set. Two bindings, because a resolve needs exactly two things
    // its persistent table cannot hold: the image it writes, which alternates
    // between the scene output and the tail target, and the sky cube, which is
    // a view into whichever room was last baked.
    //
    // The storage image is partially bound: the retro resolve is a fragment
    // pass, writes an attachment rather than a storage image, and never
    // declares that binding at all.
    std::array<VkDescriptorSetLayoutBinding, 2> resolveBindings {};
    resolveBindings[kResolveOutputBinding].binding = kResolveOutputBinding;
    resolveBindings[kResolveOutputBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    resolveBindings[kResolveOutputBinding].descriptorCount = 1;
    resolveBindings[kResolveOutputBinding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolveBindings[kResolveSkyCubeBinding].binding = kResolveSkyCubeBinding;
    resolveBindings[kResolveSkyCubeBinding].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    resolveBindings[kResolveSkyCubeBinding].descriptorCount = 1;
    resolveBindings[kResolveSkyCubeBinding].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    std::array<VkDescriptorBindingFlags, 2> resolveBindingFlags {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo resolveFlagsInfo {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    resolveFlagsInfo.bindingCount = static_cast<uint32_t>(resolveBindingFlags.size());
    resolveFlagsInfo.pBindingFlags = resolveBindingFlags.data();
    VkDescriptorSetLayoutCreateInfo resolveLayoutInfo {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    resolveLayoutInfo.pNext = &resolveFlagsInfo;
    resolveLayoutInfo.bindingCount = static_cast<uint32_t>(resolveBindings.size());
    resolveLayoutInfo.pBindings = resolveBindings.data();
    if (vkCreateDescriptorSetLayout(_device.handle(), &resolveLayoutInfo, nullptr,
                                    &_resolveLayout) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: resolve descriptor set layout creation failed");
    }

    _bindlessTextureCapacity = _device.maxBindlessSampledImages();
    if (_bindlessTextureCapacity == 0) {
        throw std::runtime_error("Vulkan: mega-draw bindless texture capacity is zero");
    }
    std::array<VkDescriptorSetLayoutBinding, 9> megaBindings {};
    // Compute alongside fragment throughout: the PBR resolve reads the material
    // records and the bindless tables from a dispatch.
    for (uint32_t i = 0; i < 3; ++i) {
        megaBindings[i].binding = i;
        megaBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        megaBindings[i].descriptorCount = 1;
        megaBindings[i].stageFlags = (i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : 0) |
                                     VK_SHADER_STAGE_FRAGMENT_BIT |
                                     VK_SHADER_STAGE_COMPUTE_BIT;
    }
    for (uint32_t i = 6; i < 9; ++i) {
        megaBindings[i].binding = i;
        megaBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        megaBindings[i].descriptorCount = 1;
        megaBindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                                     VK_SHADER_STAGE_FRAGMENT_BIT |
                                     VK_SHADER_STAGE_COMPUTE_BIT;
    }
    for (uint32_t i = 3; i < 6; ++i) {
        megaBindings[i].binding = i;
        megaBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        megaBindings[i].descriptorCount = _bindlessTextureCapacity;
        megaBindings[i].stageFlags =
            VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    }
    std::array<VkDescriptorBindingFlags, 9> megaBindingFlags {};
    for (uint32_t i = 3; i < 6; ++i) {
        megaBindingFlags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                              VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }
    // All three view shapes share the material texture id namespace. Keep each
    // table at the fixed device capacity: only the highest binding may have a
    // variable descriptor count, while any shape may contain the highest live
    // material id.
    VkDescriptorSetLayoutBindingFlagsCreateInfo megaFlagsInfo {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    megaFlagsInfo.bindingCount = static_cast<uint32_t>(megaBindingFlags.size());
    megaFlagsInfo.pBindingFlags = megaBindingFlags.data();
    VkDescriptorSetLayoutCreateInfo megaLayoutInfo {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    megaLayoutInfo.pNext = &megaFlagsInfo;
    megaLayoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    megaLayoutInfo.bindingCount = static_cast<uint32_t>(megaBindings.size());
    megaLayoutInfo.pBindings = megaBindings.data();
    if (vkCreateDescriptorSetLayout(_device.handle(), &megaLayoutInfo, nullptr,
                                    &_megaDrawLayout) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: mega-draw descriptor set layout creation failed");
    }
    _megaDrawFrames.resize(framesInFlight);
    for (auto &frame : _megaDrawFrames) {
        std::array<VkDescriptorPoolSize, 2> megaPoolSizes {{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6u * kMaxMegaDrawSetsPerFrame},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             3u * _bindlessTextureCapacity * kMaxMegaDrawSetsPerFrame},
        }};
        VkDescriptorPoolCreateInfo megaPoolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        megaPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        megaPoolInfo.maxSets = kMaxMegaDrawSetsPerFrame;
        megaPoolInfo.poolSizeCount = static_cast<uint32_t>(megaPoolSizes.size());
        megaPoolInfo.pPoolSizes = megaPoolSizes.data();
        if (vkCreateDescriptorPool(_device.handle(), &megaPoolInfo, nullptr,
                                   &frame.pool) != VK_SUCCESS) {
            throw std::runtime_error("Vulkan: mega-draw descriptor pool creation failed");
        }
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
        DescriptorWriteBuilder writes(_device.handle());
        for (int i = 0; i < kNumUniformBlocks; ++i) {
            writes.writeBuffer(_uniformSets[frame],
                               {static_cast<uint32_t>(i), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC},
                               {ring.buffer(frame), 0, kBlockSizes[i]});
        }
        writes.apply();
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

    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(_device.handle(), &samplerInfo, nullptr, &_clampSampler) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: clamped sampler creation failed");
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
        // Resolve sets come out of the same per-frame pool, so they are
        // reclaimed by the same reset. A handful per frame at most, so the
        // storage-image reserve is deliberately small.
        std::array<VkDescriptorPoolSize, 2> sizes {};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = kNumTextures * kMaxTextureSetsPerFrame;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        sizes[1].descriptorCount = kMaxResolveSetsPerFrame;

        VkDescriptorPoolCreateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = kMaxTextureSetsPerFrame + kMaxResolveSetsPerFrame;
        info.poolSizeCount = static_cast<uint32_t>(sizes.size());
        info.pPoolSizes = sizes.data();
        if (vkCreateDescriptorPool(_device.handle(), &info, nullptr, &frame.pool) != VK_SUCCESS) {
            throw std::runtime_error("Vulkan: texture descriptor pool creation failed");
        }
    }
}

VkDescriptorSet VulkanDescriptors::acquireResolveSet(int frame, const VulkanImage *output,
                                                     const VulkanImage *skyCube,
                                                     VkImageView skyView) {
    size_t key = std::hash<const void *> {}(output);
    key ^= (std::hash<const void *> {}(skyCube) ^
            std::hash<const void *> {}(reinterpret_cast<const void *>(skyView))) +
           0x9e3779b9u + (key << 6) + (key >> 2);

    auto &f = _textureFrames[frame];
    auto existing = f.byResolve.find(key);
    if (existing != f.byResolve.end()) {
        return existing->second;
    }
    if (f.byResolve.size() >= kMaxResolveSetsPerFrame) {
        throw std::runtime_error("Vulkan: too many resolve sets in one frame");
    }

    VkDescriptorSetAllocateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = f.pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &_resolveLayout;
    VkDescriptorSet set {VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(_device.handle(), &info, &set) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: resolve descriptor set allocation failed");
    }

    DescriptorWriteBuilder writes(_device.handle());
    if (output) {
        writes.writeStorageImage(toDescriptorSet(set),
                                 {kResolveOutputBinding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                 output->sampleView());
    }
    // A cube is always bound, baked or not: the fallback is black by
    // construction, so an unbaked frame samples black rather than nothing, and
    // the resolves skip the read entirely on their own flag.
    const auto *cube = skyCube ? skyCube : _defaultCube.get();
    writes.writeImage(set, {kResolveSkyCubeBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                      {cube->sampler() ? cube->sampler() : _sampler,
                       skyView && skyCube ? skyView : cube->view(),
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    writes.apply();
    f.byResolve.insert({key, set});
    return set;
}

DescriptorSet VulkanDescriptors::acquireResolveDescriptorSet(int frame, const IImage *output,
                                                             const IImage *skyCube,
                                                             ImageView skyView) {
    return toDescriptorSet(acquireResolveSet(
        frame, output ? &toVulkanImage(*output) : nullptr,
        skyCube ? &toVulkanImage(*skyCube) : nullptr,
        skyView ? toVulkanImageView(skyView) : VK_NULL_HANDLE));
}

void VulkanDescriptors::setTexture(int unit, const VulkanImage &image) {
    _standing[unit] = &image;
}

void VulkanDescriptors::beginFrame(int frame) {
    auto &mega = _megaDrawFrames[frame];
    // The frame fence has completed, so every command buffer that referenced
    // these scene-specific sets is finished before the pool is recycled.
    vkResetDescriptorPool(_device.handle(), mega.pool, 0);
    mega.sets = 0;

    auto &f = _textureFrames[frame];
    // Safe because the caller has already waited on this frame's fence.
    vkResetDescriptorPool(_device.handle(), f.pool, 0);
    f.byTexture.clear();
    f.byBindings.clear();
    f.byResolve.clear();
}

DescriptorSet VulkanDescriptors::updateMegaDrawSet(
    int frame, const GpuScene::View &scene,
    const IResources &resources) {
    auto &mega = _megaDrawFrames.at(frame);
    if (mega.sets >= kMaxMegaDrawSetsPerFrame) {
        throw std::runtime_error("Vulkan: too many merged scenes in one frame");
    }
    VkDescriptorSetAllocateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = mega.pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &_megaDrawLayout;
    VkDescriptorSet set {VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(_device.handle(), &info, &set) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: mega-draw descriptor allocation failed");
    }
    ++mega.sets;
    std::array<VkDescriptorBufferInfo, 6> buffers {{
        {toVulkanBuffer(*scene.vertices.buffer).handle(), scene.vertices.offset, scene.vertices.size},
        {toVulkanBuffer(*scene.materialIds.buffer).handle(), scene.materialIds.offset, scene.materialIds.size},
        {toVulkanBuffer(*scene.materials.buffer).handle(), scene.materials.offset, scene.materials.size},
        {toVulkanBuffer(*scene.grassCardVertices.buffer).handle(), scene.grassCardVertices.offset, scene.grassCardVertices.size},
        {toVulkanBuffer(*scene.grassCardIndices.buffer).handle(), scene.grassCardIndices.offset, scene.grassCardIndices.size},
        {toVulkanBuffer(*scene.grassCardInstances.buffer).handle(), scene.grassCardInstances.offset, scene.grassCardInstances.size},
    }};
    DescriptorWriteBuilder bufferWrites(_device.handle());
    for (uint32_t i = 0; i < 3; ++i) {
        bufferWrites.writeBuffer(set, {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, buffers[i]);
    }
    for (uint32_t i = 3; i < buffers.size(); ++i)
        bufferWrites.writeBuffer(set, {i + 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, buffers[i]);
    bufferWrites.apply();

    auto writeImages = [&](uint32_t binding, const std::vector<IResources::IndexedImage> &images) {
        DescriptorWriteBuilder writes(_device.handle());
        for (const auto &[id, image] : images) {
            if (id >= _bindlessTextureCapacity) {
                throw std::runtime_error("Vulkan: mega-draw bindless texture array exhausted");
            }
            const auto &native = toVulkanImage(*image);
            writes.writeImage(set, {binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                              {native.sampler(), native.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, id);
        }
        writes.apply();
    };
    writeImages(3, resources.uploadedTextures());
    writeImages(4, resources.uploadedTextureArrays());
    writeImages(5, resources.uploadedTextureCubes());
    return toDescriptorSet(set);
}

void VulkanDescriptors::writeTextureSet(VkDescriptorSet set, const VulkanImage *mainTex) {
    DescriptorWriteBuilder writes(_device.handle());
    for (int i = 0; i < kNumTextures; ++i) {
        auto image = (i == TextureUnits::mainTex && mainTex) ? mainTex : _standing[i];
        writes.writeImage(set, {static_cast<uint32_t>(i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                          {image->sampler() ? image->sampler() : _sampler, image->view(), sampledLayoutFor(*image)});
    }
    writes.apply();
}

VkDescriptorSet VulkanDescriptors::createPersistentTextureSet(
    const std::vector<std::pair<int, const VulkanImage *>> &bindings) {
    if (_persistentPool == VK_NULL_HANDLE) {
        VkDescriptorPoolSize size {};
        size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        size.descriptorCount = kNumTextures * kMaxPersistentTextureSets;

        VkDescriptorPoolCreateInfo info {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        // Individually freeable, because these sets outlive a frame and are
        // therefore not reclaimed by any pool reset. A rebuild that discards a
        // scene pipeline has to give its sets back or the pool is exhausted
        // after a dozen of them.
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
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

    DescriptorWriteBuilder writes(_device.handle());
    for (int i = 0; i < kNumTextures; ++i) {
        auto image = defaultFor(i, _default2D.get(), _defaultArray.get(), _defaultCube.get(),
                                  _defaultCubeArray.get());
        for (const auto &[unit, override] : bindings) {
            if (unit == i) {
                image = override;
            }
        }
        writes.writeImage(set, {static_cast<uint32_t>(i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                          {image->sampler() ? image->sampler() : _sampler, image->view(), sampledLayoutFor(*image)});
    }
    writes.apply();
    return set;
}

DescriptorSet VulkanDescriptors::createPersistentTextureSet(
    const std::vector<std::pair<int, const IImage *>> &bindings) {
    std::vector<std::pair<int, const VulkanImage *>> native;
    native.reserve(bindings.size());
    for (const auto &[unit, image] : bindings) {
        native.emplace_back(unit, image ? &toVulkanImage(*image) : nullptr);
    }
    return toDescriptorSet(createPersistentTextureSet(native));
}

void VulkanDescriptors::freePersistentTextureSet(DescriptorSet set) {
    auto native = toVulkanDescriptorSet(set);
    if (!native || _persistentPool == VK_NULL_HANDLE) {
        return;
    }
    vkFreeDescriptorSets(_device.handle(), _persistentPool, 1, &native);
}

void VulkanDescriptors::writeTextureSet(
    VkDescriptorSet set,
    const std::vector<NativeTextureBinding> &bindings) {
    DescriptorWriteBuilder writes(_device.handle());
    for (int i = 0; i < kNumTextures; ++i) {
        auto image = _standing[i];
        VkImageView view = VK_NULL_HANDLE;
        for (const auto &binding : bindings) {
            if (binding.unit == i && binding.image) {
                image = binding.image;
                view = binding.view;
            }
        }
        writes.writeImage(set, {static_cast<uint32_t>(i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                          {image->sampler() ? image->sampler() : _sampler,
                           view ? view : image->view(), sampledLayoutFor(*image)});
    }
    writes.apply();
}

VkDescriptorSet VulkanDescriptors::acquireTextureSet(
    int frame,
    const std::vector<NativeTextureBinding> &bindings) {
    size_t key = bindings.size();
    for (const auto &binding : bindings) {
        // The view is part of the identity, not a detail of the image: two
        // bindings of one cube-array image through different cube views are
        // different descriptors and must not share a cached set.
        key ^= (std::hash<const void *> {}(binding.image) ^
                std::hash<const void *> {}(reinterpret_cast<const void *>(binding.view)) ^
                static_cast<size_t>(binding.unit) * 0x9e3779b9u) +
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

DescriptorSet VulkanDescriptors::acquireTextureDescriptorSet(int frame, const IImage *mainTex) {
    return toDescriptorSet(acquireTextureSet(
        frame, mainTex ? &toVulkanImage(*mainTex) : nullptr));
}

DescriptorSet VulkanDescriptors::acquireTextureDescriptorSet(
    int frame, const std::vector<TextureBinding> &bindings) {
    std::vector<NativeTextureBinding> nativeBindings;
    nativeBindings.reserve(bindings.size());
    for (const auto &binding : bindings) {
        nativeBindings.push_back(
            {binding.unit,
             binding.image ? &toVulkanImage(*binding.image) : nullptr,
             binding.view ? toVulkanImageView(binding.view) : VK_NULL_HANDLE});
    }
    return toDescriptorSet(acquireTextureSet(frame, nativeBindings));
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
    for (auto &frame : _megaDrawFrames) {
        if (frame.pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(_device.handle(), frame.pool, nullptr);
        }
    }
    _megaDrawFrames.clear();
    if (_megaDrawLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.handle(), _megaDrawLayout, nullptr);
        _megaDrawLayout = VK_NULL_HANDLE;
    }
    _bindlessTextureCapacity = 0;
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
    if (_clampSampler != VK_NULL_HANDLE) {
        vkDestroySampler(_device.handle(), _clampSampler, nullptr);
        _clampSampler = VK_NULL_HANDLE;
    }
    if (_textureLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.handle(), _textureLayout, nullptr);
        _textureLayout = VK_NULL_HANDLE;
    }
    if (_resolveLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(_device.handle(), _resolveLayout, nullptr);
        _resolveLayout = VK_NULL_HANDLE;
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
