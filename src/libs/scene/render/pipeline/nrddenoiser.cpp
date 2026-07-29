/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include "reone/scene/render/pipeline/nrddenoiser.h"

#ifdef R_ENABLE_NRD

#include "reone/graphics/vulkan/device.h"
#include "reone/system/logutil.h"

#include <cstring>
#include <map>

using namespace reone::graphics;

namespace reone {

namespace scene {

/**
 * Generous and unmeasured on purpose: a REBLUR_DIFFUSE_SPECULAR frame is
 * around two dozen dispatches, and the slack costs kilobytes.
 */
static constexpr uint32_t kMaxDispatchesPerFrame = 64;
/** Covers every minUniformBufferOffsetAlignment in the wild. */
static constexpr VkDeviceSize kConstantAlignment = 256;

static VkFormat vkFormat(nrd::Format format) {
    switch (format) {
    case nrd::Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
    case nrd::Format::R8_SNORM: return VK_FORMAT_R8_SNORM;
    case nrd::Format::R8_UINT: return VK_FORMAT_R8_UINT;
    case nrd::Format::R8_SINT: return VK_FORMAT_R8_SINT;
    case nrd::Format::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case nrd::Format::RG8_SNORM: return VK_FORMAT_R8G8_SNORM;
    case nrd::Format::RG8_UINT: return VK_FORMAT_R8G8_UINT;
    case nrd::Format::RG8_SINT: return VK_FORMAT_R8G8_SINT;
    case nrd::Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case nrd::Format::RGBA8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case nrd::Format::RGBA8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case nrd::Format::RGBA8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case nrd::Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case nrd::Format::R16_UNORM: return VK_FORMAT_R16_UNORM;
    case nrd::Format::R16_SNORM: return VK_FORMAT_R16_SNORM;
    case nrd::Format::R16_UINT: return VK_FORMAT_R16_UINT;
    case nrd::Format::R16_SINT: return VK_FORMAT_R16_SINT;
    case nrd::Format::R16_SFLOAT: return VK_FORMAT_R16_SFLOAT;
    case nrd::Format::RG16_UNORM: return VK_FORMAT_R16G16_UNORM;
    case nrd::Format::RG16_SNORM: return VK_FORMAT_R16G16_SNORM;
    case nrd::Format::RG16_UINT: return VK_FORMAT_R16G16_UINT;
    case nrd::Format::RG16_SINT: return VK_FORMAT_R16G16_SINT;
    case nrd::Format::RG16_SFLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case nrd::Format::RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case nrd::Format::RGBA16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case nrd::Format::RGBA16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case nrd::Format::RGBA16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case nrd::Format::RGBA16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case nrd::Format::R32_UINT: return VK_FORMAT_R32_UINT;
    case nrd::Format::R32_SINT: return VK_FORMAT_R32_SINT;
    case nrd::Format::R32_SFLOAT: return VK_FORMAT_R32_SFLOAT;
    case nrd::Format::RG32_UINT: return VK_FORMAT_R32G32_UINT;
    case nrd::Format::RG32_SINT: return VK_FORMAT_R32G32_SINT;
    case nrd::Format::RG32_SFLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case nrd::Format::RGB32_UINT: return VK_FORMAT_R32G32B32_UINT;
    case nrd::Format::RGB32_SINT: return VK_FORMAT_R32G32B32_SINT;
    case nrd::Format::RGB32_SFLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case nrd::Format::RGBA32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case nrd::Format::RGBA32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
    case nrd::Format::RGBA32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case nrd::Format::R10_G10_B10_A2_UINT: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case nrd::Format::R11_G11_B10_UFLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case nrd::Format::R9_G9_B9_E5_UFLOAT: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    default:
        throw std::runtime_error("NRD: unmapped pool texture format");
    }
}

void NrdDenoiser::init() {
    const auto &instanceDesc = *nrd::GetInstanceDesc(_instance);
    const auto &libraryDesc = *nrd::GetLibraryDesc();
    const auto &offsets = libraryDesc.spirvBindingOffsets;

    VkSamplerCreateInfo samplerInfo {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    for (int i = 0; i < 2; ++i) {
        const bool linear = static_cast<nrd::Sampler>(i) == nrd::Sampler::LINEAR_CLAMP;
        samplerInfo.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        samplerInfo.minFilter = samplerInfo.magFilter;
        if (vkCreateSampler(_device.handle(), &samplerInfo, nullptr, &_samplers[i]) != VK_SUCCESS) {
            throw std::runtime_error("NRD: sampler creation failed");
        }
    }

    auto createPool = [this](const nrd::TextureDesc *descs, uint32_t count,
                             std::vector<std::unique_ptr<VulkanImage>> &pool) {
        pool.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            auto extent = glm::max(glm::ivec2(1), _extent / static_cast<int>(descs[i].downsampleFactor));
            auto image = std::make_unique<VulkanImage>(_device);
            image->initColorAttachment(extent, vkFormat(descs[i].format));
            pool.push_back(std::move(image));
        }
    };
    createPool(instanceDesc.permanentPool, instanceDesc.permanentPoolSize, _permanentPool);
    createPool(instanceDesc.transientPool, instanceDesc.transientPoolSize, _transientPool);
    _outDiffuse = std::make_unique<VulkanImage>(_device);
    _outDiffuse->initColorAttachment(_extent, VK_FORMAT_R16G16B16A16_SFLOAT);
    _outSpecular = std::make_unique<VulkanImage>(_device);
    _outSpecular->initColorAttachment(_extent, VK_FORMAT_R16G16B16A16_SFLOAT);

    // One tight layout pair per NRD pipeline. NRD may put the constant
    // buffer and samplers in a different register space than the resources;
    // each distinct space becomes its own descriptor set.
    _pipelines.resize(instanceDesc.pipelinesNum);
    for (uint32_t p = 0; p < instanceDesc.pipelinesNum; ++p) {
        const auto &pipelineDesc = instanceDesc.pipelines[p];
        std::map<uint32_t, std::vector<VkDescriptorSetLayoutBinding>> spaceBindings;
        if (pipelineDesc.hasConstantData) {
            VkDescriptorSetLayoutBinding binding {};
            binding.binding = offsets.constantBufferOffset + instanceDesc.constantBufferRegisterIndex;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            spaceBindings[instanceDesc.constantBufferAndSamplersSpaceIndex].push_back(binding);
        }
        for (uint32_t s = 0; s < instanceDesc.samplersNum; ++s) {
            VkDescriptorSetLayoutBinding binding {};
            binding.binding = offsets.samplerOffset + instanceDesc.samplersBaseRegisterIndex + s;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            binding.pImmutableSamplers = &_samplers[s];
            spaceBindings[instanceDesc.constantBufferAndSamplersSpaceIndex].push_back(binding);
        }
        // Ranges are concatenated in declaration order, each starting at the
        // base resource register within its own descriptor-type offset.
        for (uint32_t r = 0; r < pipelineDesc.resourceRangesNum; ++r) {
            const auto &range = pipelineDesc.resourceRanges[r];
            const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            for (uint32_t i = 0; i < range.descriptorsNum; ++i) {
                VkDescriptorSetLayoutBinding binding {};
                binding.binding = (storage ? offsets.storageTextureAndBufferOffset : offsets.textureOffset) +
                                  instanceDesc.resourcesBaseRegisterIndex + i;
                binding.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                 : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                binding.descriptorCount = 1;
                binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                spaceBindings[instanceDesc.resourcesSpaceIndex].push_back(binding);
            }
        }
        auto &pipeline = _pipelines[p];
        uint32_t maxSpace = 0;
        for (const auto &[space, bindings] : spaceBindings) {
            maxSpace = std::max(maxSpace, space);
        }
        std::vector<VkDescriptorSetLayout> layoutHandles(maxSpace + 1, VK_NULL_HANDLE);
        for (const auto &[space, bindings] : spaceBindings) {
            VkDescriptorSetLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();
            VkDescriptorSetLayout layout;
            if (vkCreateDescriptorSetLayout(_device.handle(), &layoutInfo, nullptr, &layout) != VK_SUCCESS) {
                throw std::runtime_error("NRD: descriptor layout creation failed");
            }
            pipeline.setLayouts.push_back({space, layout});
            layoutHandles[space] = layout;
        }
        // Spaces must be contiguous for the pipeline layout; NRD's defaults
        // use 0 (and possibly 1). A gap would need a filler layout.
        for (auto handle : layoutHandles) {
            if (handle == VK_NULL_HANDLE) {
                throw std::runtime_error("NRD: non-contiguous register spaces");
            }
        }
        VkPipelineLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = static_cast<uint32_t>(layoutHandles.size());
        layoutInfo.pSetLayouts = layoutHandles.data();
        if (vkCreatePipelineLayout(_device.handle(), &layoutInfo, nullptr, &pipeline.layout) != VK_SUCCESS) {
            throw std::runtime_error("NRD: pipeline layout creation failed");
        }
        VkShaderModuleCreateInfo moduleInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = static_cast<size_t>(pipelineDesc.computeShaderSPIRV.size);
        moduleInfo.pCode = static_cast<const uint32_t *>(pipelineDesc.computeShaderSPIRV.bytecode);
        VkShaderModule module;
        if (vkCreateShaderModule(_device.handle(), &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            throw std::runtime_error("NRD: shader module creation failed");
        }
        VkComputePipelineCreateInfo pipelineInfo {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = instanceDesc.shaderEntryPoint;
        pipelineInfo.layout = pipeline.layout;
        const auto result = vkCreateComputePipelines(_device.handle(), VK_NULL_HANDLE, 1,
                                                     &pipelineInfo, nullptr, &pipeline.handle);
        vkDestroyShaderModule(_device.handle(), module, nullptr);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("NRD: compute pipeline creation failed");
        }
    }

    // Per-frame descriptor pools, reset wholesale at the top of each denoise.
    for (auto &pool : _descriptorPools) {
        VkDescriptorPoolSize sizes[] {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxDispatchesPerFrame},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * kMaxDispatchesPerFrame},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 * kMaxDispatchesPerFrame},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16 * kMaxDispatchesPerFrame},
        };
        VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 2 * kMaxDispatchesPerFrame;
        poolInfo.poolSizeCount = 4;
        poolInfo.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(_device.handle(), &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            throw std::runtime_error("NRD: descriptor pool creation failed");
        }
    }

    _constantSlotSize = (instanceDesc.constantBufferMaxDataSize + kConstantAlignment - 1) &
                        ~(kConstantAlignment - 1);
    _constantSlotsPerFrame = kMaxDispatchesPerFrame;
    _constants = std::make_unique<VulkanBuffer>(_device);
    _constants->initHostVisible(_constantSlotSize * _constantSlotsPerFrame * 2,
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    info("NRD: " + std::to_string(_pipelines.size()) + " pipelines built, " +
         std::to_string(_permanentPool.size() + _transientPool.size()) + " pool textures");
}

void NrdDenoiser::deinit() {
    for (auto &pipeline : _pipelines) {
        if (pipeline.handle) vkDestroyPipeline(_device.handle(), pipeline.handle, nullptr);
        if (pipeline.layout) vkDestroyPipelineLayout(_device.handle(), pipeline.layout, nullptr);
        for (auto &[space, layout] : pipeline.setLayouts) {
            vkDestroyDescriptorSetLayout(_device.handle(), layout, nullptr);
        }
    }
    _pipelines.clear();
    for (auto &pool : _descriptorPools) {
        if (pool) vkDestroyDescriptorPool(_device.handle(), pool, nullptr);
        pool = VK_NULL_HANDLE;
    }
    for (auto &sampler : _samplers) {
        if (sampler) vkDestroySampler(_device.handle(), sampler, nullptr);
        sampler = VK_NULL_HANDLE;
    }
    _permanentPool.clear();
    _transientPool.clear();
    _outDiffuse.reset();
    _outSpecular.reset();
    _constants.reset();
    _poolTransitioned = false;
    _hasHistory = false;
}

VkImageView NrdDenoiser::viewFor(const nrd::ResourceDesc &resource, const Inputs &inputs) const {
    switch (resource.type) {
    case nrd::ResourceType::IN_MV: return inputs.motion;
    case nrd::ResourceType::IN_NORMAL_ROUGHNESS: return inputs.normalRoughness;
    case nrd::ResourceType::IN_VIEWZ: return inputs.viewZ;
    case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST: return inputs.diffRadianceHitDist;
    case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST: return inputs.specRadianceHitDist;
    case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST: return _outDiffuse->view();
    case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST: return _outSpecular->view();
    case nrd::ResourceType::PERMANENT_POOL: return _permanentPool[resource.indexInPool]->view();
    case nrd::ResourceType::TRANSIENT_POOL: return _transientPool[resource.indexInPool]->view();
    default:
        throw std::runtime_error("NRD: dispatch references an unprovided resource type " +
                                 std::to_string(static_cast<int>(resource.type)));
    }
}

void NrdDenoiser::denoise(VkCommandBuffer cmd,
                          int frameIndex,
                          const Inputs &inputs,
                          const Tuning &tuning,
                          const glm::mat4 &view,
                          const glm::mat4 &projection,
                          const glm::vec2 &jitter,
                          uint32_t frameNumber,
                          bool restartHistory) {
    if (!_poolTransitioned) {
        // Everything NRD touches lives in GENERAL for its whole life; the
        // per-dispatch hazard is handled by execution barriers, not layouts.
        std::vector<VkImageMemoryBarrier2> barriers;
        auto add = [&barriers](VkImage image) {
            VkImageMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barriers.push_back(barrier);
        };
        for (auto &image : _permanentPool) add(image->handle());
        for (auto &image : _transientPool) add(image->handle());
        add(_outDiffuse->handle());
        add(_outSpecular->handle());
        VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dependency);
        _poolTransitioned = true;
    }

    // A warp is not camera motion: reprojecting across it drags the previous
    // module's history over the new scene. A teleport-sized jump resets the
    // accumulation instead. Twenty units comfortably exceeds any legitimate
    // per-frame camera move and is far under any warp.
    const glm::vec3 cameraPosition = glm::vec3(glm::inverse(view)[3]);
    if (_hasHistory && glm::distance(cameraPosition, _prevCameraPosition) > 20.0f) {
        restartHistory = true;
    }
    _prevCameraPosition = cameraPosition;

    nrd::CommonSettings common {};
    std::memcpy(common.viewToClipMatrix, &projection[0][0], sizeof(float) * 16);
    std::memcpy(common.worldToViewMatrix, &view[0][0], sizeof(float) * 16);
    const auto &prevProjection = _hasHistory ? _prevProjection : projection;
    const auto &prevView = _hasHistory ? _prevView : view;
    std::memcpy(common.viewToClipMatrixPrev, &prevProjection[0][0], sizeof(float) * 16);
    std::memcpy(common.worldToViewMatrixPrev, &prevView[0][0], sizeof(float) * 16);
    common.motionVectorScale[0] = 1.0f;
    common.motionVectorScale[1] = 1.0f;
    common.motionVectorScale[2] = 1.0f;
    common.isMotionVectorInWorldSpace = true;
    common.cameraJitter[0] = jitter.x;
    common.cameraJitter[1] = jitter.y;
    common.cameraJitterPrev[0] = _hasHistory ? _prevJitter.x : jitter.x;
    common.cameraJitterPrev[1] = _hasHistory ? _prevJitter.y : jitter.y;
    common.resourceSize[0] = static_cast<uint16_t>(_extent.x);
    common.resourceSize[1] = static_cast<uint16_t>(_extent.y);
    common.resourceSizePrev[0] = common.resourceSize[0];
    common.resourceSizePrev[1] = common.resourceSize[1];
    common.rectSize[0] = common.resourceSize[0];
    common.rectSize[1] = common.resourceSize[1];
    common.rectSizePrev[0] = common.resourceSize[0];
    common.rectSizePrev[1] = common.resourceSize[1];
    common.frameIndex = frameNumber;
    common.accumulationMode = (restartHistory || !_hasHistory) ? nrd::AccumulationMode::CLEAR_AND_RESTART
                                                               : nrd::AccumulationMode::CONTINUE;
    common.disocclusionThreshold = glm::clamp(tuning.disocclusionThreshold, 0.001f, 0.2f);
    if (nrd::SetCommonSettings(_instance, common) != nrd::Result::SUCCESS) {
        warn("NRD: SetCommonSettings failed");
        return;
    }
    nrd::ReblurSettings reblur {};
    reblur.maxAccumulatedFrameNum = static_cast<uint32_t>(glm::max(0, tuning.maxAccumulatedFrames));
    reblur.maxFastAccumulatedFrameNum = static_cast<uint32_t>(glm::max(0, tuning.maxFastAccumulatedFrames));
    reblur.maxStabilizedFrameNum = static_cast<uint32_t>(glm::max(0, tuning.maxStabilizedFrames));
    reblur.historyFixFrameNum = static_cast<uint32_t>(glm::max(0, tuning.historyFixFrames));
    reblur.diffusePrepassBlurRadius = glm::max(0.0f, tuning.diffusePrepassBlurRadius);
    reblur.specularPrepassBlurRadius = glm::max(0.0f, tuning.specularPrepassBlurRadius);
    reblur.minBlurRadius = glm::max(0.0f, tuning.minBlurRadius);
    reblur.maxBlurRadius = glm::max(tuning.minBlurRadius, tuning.maxBlurRadius);
    reblur.lobeAngleFraction = glm::clamp(tuning.lobeAngleFraction, 0.0f, 1.0f);
    reblur.roughnessFraction = glm::clamp(tuning.roughnessFraction, 0.0f, 1.0f);
    reblur.planeDistanceSensitivity = glm::clamp(tuning.planeDistanceSensitivity, 0.001f, 1.0f);
    reblur.enableAntiFirefly = tuning.antiFirefly;
    if (nrd::SetDenoiserSettings(_instance, 0, &reblur) != nrd::Result::SUCCESS) {
        warn("NRD: SetDenoiserSettings failed");
        return;
    }
    _prevView = view;
    _prevProjection = projection;
    _prevJitter = jitter;
    _hasHistory = true;

    const nrd::Identifier identifier = 0;
    const nrd::DispatchDesc *dispatches = nullptr;
    uint32_t dispatchCount = 0;
    if (nrd::GetComputeDispatches(_instance, &identifier, 1, dispatches, dispatchCount) != nrd::Result::SUCCESS) {
        warn("NRD: GetComputeDispatches failed");
        return;
    }
    if (dispatchCount > kMaxDispatchesPerFrame) {
        throw std::runtime_error("NRD: dispatch count exceeds the frame budget");
    }

    const auto pool = _descriptorPools[frameIndex];
    vkResetDescriptorPool(_device.handle(), pool, 0);
    const auto &instanceDesc = *nrd::GetInstanceDesc(_instance);
    const auto &offsets = nrd::GetLibraryDesc()->spirvBindingOffsets;
    auto *constantBase = static_cast<uint8_t *>(_constants->mapped()) +
                         static_cast<VkDeviceSize>(frameIndex) * _constantSlotSize * _constantSlotsPerFrame;

    // The conservative hazard model: every dispatch's writes are made visible
    // to the next dispatch's reads and writes. NRD's graph is mostly linear;
    // finer tracking is a measured optimization, not a correctness need.
    VkMemoryBarrier2 computeBarrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    computeBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    computeBarrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    computeBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    computeBarrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                   VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    VkDependencyInfo computeDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    computeDependency.memoryBarrierCount = 1;
    computeDependency.pMemoryBarriers = &computeBarrier;

    for (uint32_t d = 0; d < dispatchCount; ++d) {
        const auto &dispatch = dispatches[d];
        const auto &pipeline = _pipelines[dispatch.pipelineIndex];
        const auto &pipelineDesc = instanceDesc.pipelines[dispatch.pipelineIndex];

        std::vector<VkDescriptorSet> sets;
        std::vector<uint32_t> setSpaces;
        for (const auto &[space, layout] : pipeline.setLayouts) {
            VkDescriptorSetAllocateInfo alloc {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = pool;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &layout;
            VkDescriptorSet set;
            if (vkAllocateDescriptorSets(_device.handle(), &alloc, &set) != VK_SUCCESS) {
                throw std::runtime_error("NRD: per-dispatch descriptor allocation failed");
            }
            sets.push_back(set);
            setSpaces.push_back(space);
        }
        auto setForSpace = [&](uint32_t space) {
            for (size_t i = 0; i < setSpaces.size(); ++i) {
                if (setSpaces[i] == space) return sets[i];
            }
            throw std::runtime_error("NRD: dispatch resource in undeclared space");
        };

        std::vector<VkDescriptorImageInfo> imageInfos;
        imageInfos.reserve(dispatch.resourcesNum);
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(dispatch.resourcesNum + 1);
        uint32_t resourceIndex = 0;
        for (uint32_t r = 0; r < pipelineDesc.resourceRangesNum; ++r) {
            const auto &range = pipelineDesc.resourceRanges[r];
            const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            for (uint32_t i = 0; i < range.descriptorsNum; ++i, ++resourceIndex) {
                const auto &resource = dispatch.resources[resourceIndex];
                imageInfos.push_back({VK_NULL_HANDLE, viewFor(resource, inputs), VK_IMAGE_LAYOUT_GENERAL});
                VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = setForSpace(instanceDesc.resourcesSpaceIndex);
                write.dstBinding = (storage ? offsets.storageTextureAndBufferOffset : offsets.textureOffset) +
                                   instanceDesc.resourcesBaseRegisterIndex + i;
                write.descriptorCount = 1;
                write.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                               : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                write.pImageInfo = &imageInfos.back();
                writes.push_back(write);
            }
        }
        VkDescriptorBufferInfo constantInfo {};
        if (pipelineDesc.hasConstantData && dispatch.constantBufferDataSize > 0) {
            auto *slot = constantBase + static_cast<VkDeviceSize>(d) * _constantSlotSize;
            std::memcpy(slot, dispatch.constantBufferData, dispatch.constantBufferDataSize);
            constantInfo.buffer = _constants->handle();
            constantInfo.offset = static_cast<VkDeviceSize>(frameIndex) * _constantSlotSize * _constantSlotsPerFrame +
                                  static_cast<VkDeviceSize>(d) * _constantSlotSize;
            constantInfo.range = dispatch.constantBufferDataSize;
            VkWriteDescriptorSet write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = setForSpace(instanceDesc.constantBufferAndSamplersSpaceIndex);
            write.dstBinding = offsets.constantBufferOffset + instanceDesc.constantBufferRegisterIndex;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.pBufferInfo = &constantInfo;
            writes.push_back(write);
        }
        vkUpdateDescriptorSets(_device.handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        vkCmdPipelineBarrier2(cmd, &computeDependency);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle);
        for (size_t s = 0; s < sets.size(); ++s) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout,
                                    setSpaces[s], 1, &sets[s], 0, nullptr);
        }
        vkCmdDispatch(cmd, dispatch.gridWidth, dispatch.gridHeight, 1);
    }
    // The composite reads the outputs with sampled or storage access; settle
    // the last writes before anything downstream.
    vkCmdPipelineBarrier2(cmd, &computeDependency);
}

} // namespace scene

} // namespace reone

#endif
