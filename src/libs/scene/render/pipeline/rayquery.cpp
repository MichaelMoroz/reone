/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/scene/render/pipeline/rayquery.h"

#include <algorithm>

#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/textureutil.h"
#include "reone/graphics/uniforms.h"

#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/mesh.h"
#include "reone/graphics/vulkan/pipeline.h"
#include "reone/graphics/vulkan/pipelinecache.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/scene/node/model.h"

#ifdef R_ENABLE_NRD
#include <NRD.h>
#endif
#include "reone/system/logutil.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <map>

#include <glm/gtc/matrix_transform.hpp>

using namespace reone::graphics;

namespace reone::scene {
namespace {

using InstanceMaterial = GpuScene::InstanceMaterial;
using MergedVertex = GpuScene::MergedVertex;

// Sky cubemap face resolution. Measured on danm14ab against the geometry sky
// it replaces, as a ratio of surviving horizontal detail: 512 keeps 0.59,
// 1024 keeps 0.73, 2048 keeps 0.77 for four times the memory. The curve is
// already flattening at 1024, so the rest of the gap is resampling and
// filtering rather than resolution, and paying 192 MB for it buys little.
// Frame time is flat across all three.
static constexpr uint32_t kSkyCubeSize = 1024;

struct TraceStats {
    uint32_t secondaryRays {0};
    uint32_t secondaryMisses {0};
    uint32_t survivingLights {0};
    uint32_t primaryHits {0};
    uint32_t shadowRays {0};
};

VkDeviceAddress alignedAddress(VkDeviceAddress address, VkDeviceSize alignment) {
    return (address + alignment - 1) & ~(alignment - 1);
}

VkTransformMatrixKHR instanceTransform(const glm::mat4 &m) {
    VkTransformMatrixKHR out {};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            out.matrix[row][column] = m[column][row];
        }
    }
    return out;
}

VkDeviceSize grownCapacity(VkDeviceSize current, VkDeviceSize required, VkDeviceSize minimum) {
    if (current != 0 && required <= current)
        return current;
    VkDeviceSize capacity = std::max(current, minimum);
    while (capacity < required) {
        if (capacity > std::numeric_limits<VkDeviceSize>::max() / 2) {
            throw std::runtime_error("Vulkan: acceleration-structure capacity exceeds device-size range");
        }
        capacity *= 2;
    }
    return capacity;
}
} // namespace

RayQueryPipeline::RayQueryPipeline(VulkanRenderer &renderer,
                                   glm::ivec2 extent,
                                   GraphicsOptions &options,
                                   GpuScene &gpuScene) :
    _renderer(renderer), _options(options), _extent(extent),
    _gpuScene(gpuScene) {}

void RayQueryPipeline::init() {
    if (_inited)
        return;
    auto &device = _renderer.device();
    _bindlessTextureCapacity = device.maxBindlessSampledImages();
    if (_bindlessTextureCapacity == 0) {
        throw std::runtime_error("Vulkan: ray-query bindless texture capacity is zero");
    }
    VkDescriptorSetLayoutBinding bindings[10] {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    // Ranges into the one merged geometry buffer: vertices, indices, material ids.
    for (uint32_t i = 4; i <= 6; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    }
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[7].descriptorCount = _bindlessTextureCapacity;
    bindings[7].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    bindings[8].binding = 8;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[8].descriptorCount = _bindlessTextureCapacity;
    bindings[8].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    bindings[9].binding = 9;
    bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    VkDescriptorBindingFlags bindingFlags[10] {};
    bindingFlags[7] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    // The sky cube occupies binding 9, so the two bindless ranges keep their
    // fixed device-limit allocation rather than using Vulkan's highest-binding
    // variable-descriptor-count rule.
    bindingFlags[8] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bindingFlagsInfo.bindingCount = 10;
    bindingFlagsInfo.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.pNext = &bindingFlagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 10;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device.handle(), &layoutInfo, nullptr, &_layout) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: ray-query descriptor layout creation failed");

    VkDescriptorPoolSize sizes[] {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
                                  {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   4 * _bindlessTextureCapacity + 2}};
    VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 4;
    poolInfo.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device.handle(), &poolInfo, nullptr, &_pool) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: ray-query descriptor pool creation failed");
    VkDescriptorSetAllocateInfo alloc {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    std::array<VkDescriptorSetLayout, 2> setLayouts {_layout, _layout};
    alloc.descriptorPool = _pool;
    alloc.descriptorSetCount = static_cast<uint32_t>(setLayouts.size());
    alloc.pSetLayouts = setLayouts.data();
    if (vkAllocateDescriptorSets(device.handle(), &alloc, _sets.data()) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: ray-query descriptor allocation failed");

    // A descriptor is required even when no room qualifies. Sampling is gated
    // in the shader, but this black cube keeps the descriptor type valid.
    const float black[4] {0.0f, 0.0f, 0.0f, 1.0f};
    _skyFallbackCube = std::make_unique<VulkanImage>(device);
    _skyFallbackCube->initSampledLayered({1, 1}, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         kNumCubeFaces, true, black);
    _skyFallbackCube->setSampler(
        _renderer.resources().samplers().get(getTextureProperties(TextureUsage::ColorBuffer)));

    // The NRD output split: seven storage images in their own set, because
    // the main set's bindless arrays hold the variable-descriptor-count slot
    // and Vulkan allows nothing above it. Plain pool, static writes - the
    // images never change identity within a pipeline lifetime.
    {
        std::array<VkDescriptorSetLayoutBinding, kNumAuxImages> auxBindings {};
        for (uint32_t i = 0; i < kNumAuxImages; ++i) {
            auxBindings[i].binding = i;
            auxBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            auxBindings[i].descriptorCount = 1;
            auxBindings[i].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        }
        VkDescriptorSetLayoutCreateInfo auxLayoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        auxLayoutInfo.bindingCount = kNumAuxImages;
        auxLayoutInfo.pBindings = auxBindings.data();
        if (vkCreateDescriptorSetLayout(device.handle(), &auxLayoutInfo, nullptr, &_auxLayout) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: trace output descriptor layout creation failed");
        VkDescriptorPoolSize auxPoolSize {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kNumAuxImages};
        VkDescriptorPoolCreateInfo auxPoolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        auxPoolInfo.maxSets = 2;
        auxPoolInfo.poolSizeCount = 1;
        auxPoolInfo.pPoolSizes = &auxPoolSize;
        if (vkCreateDescriptorPool(device.handle(), &auxPoolInfo, nullptr, &_auxPool) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: trace output descriptor pool creation failed");
        std::array<VkDescriptorSetLayout, 2> auxSetLayouts {_auxLayout, _auxLayout};
        VkDescriptorSetAllocateInfo auxAlloc {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        auxAlloc.descriptorPool = _auxPool;
        auxAlloc.descriptorSetCount = 2;
        auxAlloc.pSetLayouts = auxSetLayouts.data();
        if (vkAllocateDescriptorSets(device.handle(), &auxAlloc, _auxSets.data()) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: trace output descriptor allocation failed");
        // Formats mirror the shader's declarations; normal/roughness rides
        // RGBA16F, the FP form NRD's RGBA16_SNORM encoding accepts.
        static constexpr VkFormat kAuxFormats[kNumAuxImages] {
            VK_FORMAT_R16G16B16A16_SFLOAT, // diffuse radiance + hit dist
            VK_FORMAT_R16G16B16A16_SFLOAT, // specular radiance + hit dist
            VK_FORMAT_R16G16B16A16_SFLOAT, // normal + roughness
            VK_FORMAT_R32_SFLOAT,          // viewZ
            VK_FORMAT_R16G16B16A16_SFLOAT, // motion
            VK_FORMAT_R16G16B16A16_SFLOAT, // noise-free
            VK_FORMAT_R16G16B16A16_SFLOAT, // diffuse material factor
            VK_FORMAT_R32_SFLOAT,          // device depth, for the upscaler
            VK_FORMAT_R16G16B16A16_SFLOAT, // screen-space motion, for the upscaler
            VK_FORMAT_R16G16B16A16_SFLOAT, // specular material factor
            VK_FORMAT_R8G8B8A8_UNORM,      // canonical raster/tracer diffuse
            VK_FORMAT_R8G8B8A8_UNORM,      // canonical packed eye normal
            VK_FORMAT_R32_SFLOAT,          // canonical positive linear view depth
            VK_FORMAT_R16G16_SFLOAT,       // canonical current-minus-previous UV motion
        };
        std::array<VkDescriptorImageInfo, 2 * kNumAuxImages> auxImageInfos {};
        std::array<VkWriteDescriptorSet, 2 * kNumAuxImages> auxWrites {};
        for (int frame = 0; frame < 2; ++frame) {
            for (int i = 0; i < kNumAuxImages; ++i) {
                auto image = std::make_unique<VulkanImage>(device);
                image->initColorAttachment(_extent, kAuxFormats[i]);
                const auto flatIndex = frame * kNumAuxImages + i;
                auxImageInfos[flatIndex] = {VK_NULL_HANDLE, image->view(), VK_IMAGE_LAYOUT_GENERAL};
                auxWrites[flatIndex].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                auxWrites[flatIndex].dstSet = _auxSets[frame];
                auxWrites[flatIndex].dstBinding = i;
                auxWrites[flatIndex].descriptorCount = 1;
                auxWrites[flatIndex].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                auxWrites[flatIndex].pImageInfo = &auxImageInfos[flatIndex];
                _auxImages[frame][i] = std::move(image);
            }
        }
        vkUpdateDescriptorSets(device.handle(), static_cast<uint32_t>(auxWrites.size()),
                               auxWrites.data(), 0, nullptr);
    }

    auto spirv = readSpirV(_renderer.shaderDir() / "rayquery.spv");
    VkShaderModuleCreateInfo moduleInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = spirv.size() * sizeof(uint32_t);
    moduleInfo.pCode = spirv.data();
    VkShaderModule module;
    if (vkCreateShaderModule(device.handle(), &moduleInfo, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: ray-query shader module creation failed");
    VkDescriptorSetLayout layouts[] {_renderer.descriptors().uniformLayout(), _layout, _auxLayout};
    VkPushConstantRange pushConstants {};
    pushConstants.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    pushConstants.size = sizeof(TracePushConstants);
    VkPipelineLayoutCreateInfo pipelineLayout {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayout.setLayoutCount = 3;
    pipelineLayout.pSetLayouts = layouts;
    pipelineLayout.pushConstantRangeCount = 1;
    pipelineLayout.pPushConstantRanges = &pushConstants;
    if (vkCreatePipelineLayout(device.handle(), &pipelineLayout, nullptr, &_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: ray-query pipeline layout creation failed");
    }
    VkPipelineShaderStageCreateInfo stage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stage.module = module;
    stage.pName = "main";
    VkRayTracingShaderGroupCreateInfoKHR group {
        VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    group.generalShader = 0;
    group.closestHitShader = VK_SHADER_UNUSED_KHR;
    group.anyHitShader = VK_SHADER_UNUSED_KHR;
    group.intersectionShader = VK_SHADER_UNUSED_KHR;
    VkRayTracingPipelineCreateInfoKHR pipeline {
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    pipeline.stageCount = 1;
    pipeline.pStages = &stage;
    pipeline.groupCount = 1;
    pipeline.pGroups = &group;
    pipeline.maxPipelineRayRecursionDepth = 1;
    pipeline.layout = _pipelineLayout;
    if (vkCreateRayTracingPipelinesKHR(device.handle(), VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeline,
                                       nullptr, &_pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: ray-query ray-tracing pipeline creation failed");
    }
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &rtProperties;
    vkGetPhysicalDeviceProperties2(device.physicalDevice(), &properties);
    const auto alignUp = [](VkDeviceSize value, VkDeviceSize alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    };
    const VkDeviceSize recordSize = alignUp(rtProperties.shaderGroupHandleSize,
                                            rtProperties.shaderGroupHandleAlignment);
    const VkDeviceSize allocationSize = recordSize + rtProperties.shaderGroupBaseAlignment - 1;
    std::vector<uint8_t> handle(rtProperties.shaderGroupHandleSize);
    if (vkGetRayTracingShaderGroupHandlesKHR(device.handle(), _pipeline, 0, 1, handle.size(),
                                             handle.data()) != VK_SUCCESS) {
        vkDestroyShaderModule(device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: raygen shader-group handle query failed");
    }
    _raygenSbt = std::make_unique<VulkanBuffer>(device);
    _raygenSbt->initHostVisible(
        allocationSize, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    const VkDeviceAddress sbtAddress = alignUp(_raygenSbt->deviceAddress(),
                                               rtProperties.shaderGroupBaseAlignment);
    const VkDeviceSize sbtOffset = sbtAddress - _raygenSbt->deviceAddress();
    std::memcpy(static_cast<uint8_t *>(_raygenSbt->mapped()) + sbtOffset, handle.data(),
                handle.size());
    _raygenSbtRegion.deviceAddress = sbtAddress;
    _raygenSbtRegion.stride = recordSize;
    _raygenSbtRegion.size = recordSize;
    vkDestroyShaderModule(device.handle(), module, nullptr);
    device.setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(_pipeline), "rayquery:primaryRay");

    _gpuScene.init(_renderer);
#ifdef R_ENABLE_NRD
    {
        // Stage 1 of the NRD integration: prove the library is linked, its
        // instance comes up, and its resource demands are known. The
        // dispatches themselves arrive with the output split.
        const nrd::LibraryDesc &libraryDesc = *nrd::GetLibraryDesc();
        nrd::DenoiserDesc denoiserDesc {0, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR};
        nrd::InstanceCreationDesc creationDesc {};
        creationDesc.denoisers = &denoiserDesc;
        creationDesc.denoisersNum = 1;
        nrd::Instance *instance = nullptr;
        if (nrd::CreateInstance(creationDesc, instance) == nrd::Result::SUCCESS) {
            _nrdInstance = instance;
            const nrd::InstanceDesc &instanceDesc = *nrd::GetInstanceDesc(*instance);
            info("NRD " + std::to_string(libraryDesc.versionMajor) + "." +
                 std::to_string(libraryDesc.versionMinor) + "." +
                 std::to_string(libraryDesc.versionBuild) + " up: " +
                 std::to_string(instanceDesc.pipelinesNum) + " pipelines, " +
                 std::to_string(instanceDesc.permanentPoolSize) + " permanent + " +
                 std::to_string(instanceDesc.transientPoolSize) + " transient pool textures");
            _nrdDenoiser = std::make_unique<NrdDenoiser>(device, *instance, _extent);
            _nrdDenoiser->init();

            // Seven: output, noise-free, diffuse and specular factors, denoised diffuse and specular,
            // viewZ. Must match the binding list in slang/nrd_composite.slang.
            constexpr uint32_t kCompositeBindingCount = 7;
            VkDescriptorSetLayoutBinding compositeBindings[kCompositeBindingCount] {};
            for (uint32_t i = 0; i < kCompositeBindingCount; ++i) {
                compositeBindings[i].binding = i;
                compositeBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                compositeBindings[i].descriptorCount = 1;
                compositeBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo compositeLayoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            compositeLayoutInfo.bindingCount = kCompositeBindingCount;
            compositeLayoutInfo.pBindings = compositeBindings;
            if (vkCreateDescriptorSetLayout(device.handle(), &compositeLayoutInfo, nullptr, &_compositeLayout) != VK_SUCCESS)
                throw std::runtime_error("Vulkan: NRD composite layout creation failed");
            VkDescriptorPoolSize compositePoolSize {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kCompositeBindingCount};
            VkDescriptorPoolCreateInfo compositePoolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            compositePoolInfo.maxSets = 2;
            compositePoolInfo.poolSizeCount = 1;
            compositePoolInfo.pPoolSizes = &compositePoolSize;
            if (vkCreateDescriptorPool(device.handle(), &compositePoolInfo, nullptr, &_compositePool) != VK_SUCCESS)
                throw std::runtime_error("Vulkan: NRD composite pool creation failed");
            std::array<VkDescriptorSetLayout, 2> compositeSetLayouts {_compositeLayout, _compositeLayout};
            VkDescriptorSetAllocateInfo compositeAlloc {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            compositeAlloc.descriptorPool = _compositePool;
            compositeAlloc.descriptorSetCount = 2;
            compositeAlloc.pSetLayouts = compositeSetLayouts.data();
            if (vkAllocateDescriptorSets(device.handle(), &compositeAlloc, _compositeSets.data()) != VK_SUCCESS)
                throw std::runtime_error("Vulkan: NRD composite set allocation failed");
            VkPushConstantRange compositePush {};
            compositePush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            compositePush.size = 3 * sizeof(uint32_t);
            // Set 0 is the shared uniform block, kept because the pipeline
            // layout is shared with the rest of the traced passes.
            VkDescriptorSetLayout compositeLayouts[] {_renderer.descriptors().uniformLayout(), _compositeLayout};
            VkPipelineLayoutCreateInfo compositePipelineLayout {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            compositePipelineLayout.setLayoutCount = 2;
            compositePipelineLayout.pSetLayouts = compositeLayouts;
            compositePipelineLayout.pushConstantRangeCount = 1;
            compositePipelineLayout.pPushConstantRanges = &compositePush;
            if (vkCreatePipelineLayout(device.handle(), &compositePipelineLayout, nullptr, &_compositePipelineLayout) != VK_SUCCESS)
                throw std::runtime_error("Vulkan: NRD composite pipeline layout creation failed");
            auto compositeSpirv = readSpirV(_renderer.shaderDir() / "nrd_composite.spv");
            VkShaderModuleCreateInfo compositeModuleInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            compositeModuleInfo.codeSize = compositeSpirv.size() * sizeof(uint32_t);
            compositeModuleInfo.pCode = compositeSpirv.data();
            VkShaderModule compositeModule;
            if (vkCreateShaderModule(device.handle(), &compositeModuleInfo, nullptr, &compositeModule) != VK_SUCCESS)
                throw std::runtime_error("Vulkan: NRD composite shader module creation failed");
            VkComputePipelineCreateInfo compositePipeline {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            compositePipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            compositePipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            compositePipeline.stage.module = compositeModule;
            compositePipeline.stage.pName = "main";
            compositePipeline.layout = _compositePipelineLayout;
            const auto compositeResult = vkCreateComputePipelines(device.handle(), VK_NULL_HANDLE, 1,
                                                                  &compositePipeline, nullptr, &_compositePipeline);
            vkDestroyShaderModule(device.handle(), compositeModule, nullptr);
            if (compositeResult != VK_SUCCESS)
                throw std::runtime_error("Vulkan: NRD composite pipeline creation failed");
        } else {
            warn("NRD instance creation failed; denoising stays unavailable");
        }
    }
#endif
#ifdef R_ENABLE_FSR
    if (_options.ptFsr) {
        // Both at render resolution: NativeAA does not change the size, and the
        // composite/tonemap pair either side of FSR work on the same grid.
        _fsrColor = std::make_unique<VulkanImage>(device);
        _fsrColor->initColorAttachment(_extent, VK_FORMAT_R16G16B16A16_SFLOAT);
        _fsrOutput = std::make_unique<VulkanImage>(device);
        _fsrOutput->initColorAttachment(_extent, VK_FORMAT_R16G16B16A16_SFLOAT);

        std::array<VkDescriptorSetLayoutBinding, 2> tonemapBindings {};
        for (uint32_t i = 0; i < 2; ++i) {
            tonemapBindings[i].binding = i;
            tonemapBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            tonemapBindings[i].descriptorCount = 1;
            tonemapBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo tonemapLayoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        tonemapLayoutInfo.bindingCount = 2;
        tonemapLayoutInfo.pBindings = tonemapBindings.data();
        if (vkCreateDescriptorSetLayout(device.handle(), &tonemapLayoutInfo, nullptr, &_tonemapLayout) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: tonemap descriptor layout creation failed");
        VkDescriptorPoolSize tonemapPoolSize {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * 2};
        VkDescriptorPoolCreateInfo tonemapPoolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        tonemapPoolInfo.maxSets = 2;
        tonemapPoolInfo.poolSizeCount = 1;
        tonemapPoolInfo.pPoolSizes = &tonemapPoolSize;
        if (vkCreateDescriptorPool(device.handle(), &tonemapPoolInfo, nullptr, &_tonemapPool) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: tonemap descriptor pool creation failed");
        std::array<VkDescriptorSetLayout, 2> tonemapSetLayouts {_tonemapLayout, _tonemapLayout};
        VkDescriptorSetAllocateInfo tonemapAlloc {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        tonemapAlloc.descriptorPool = _tonemapPool;
        tonemapAlloc.descriptorSetCount = 2;
        tonemapAlloc.pSetLayouts = tonemapSetLayouts.data();
        if (vkAllocateDescriptorSets(device.handle(), &tonemapAlloc, _tonemapSets.data()) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: tonemap set allocation failed");
        VkPushConstantRange tonemapPush {};
        tonemapPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        tonemapPush.size = 2 * sizeof(uint32_t);
        VkDescriptorSetLayout tonemapLayouts[] {_renderer.descriptors().uniformLayout(), _tonemapLayout};
        VkPipelineLayoutCreateInfo tonemapPipelineLayout {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        tonemapPipelineLayout.setLayoutCount = 2;
        tonemapPipelineLayout.pSetLayouts = tonemapLayouts;
        tonemapPipelineLayout.pushConstantRangeCount = 1;
        tonemapPipelineLayout.pPushConstantRanges = &tonemapPush;
        if (vkCreatePipelineLayout(device.handle(), &tonemapPipelineLayout, nullptr, &_tonemapPipelineLayout) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: tonemap pipeline layout creation failed");
        auto tonemapSpirv = readSpirV(_renderer.shaderDir() / "pt_tonemap.spv");
        VkShaderModuleCreateInfo tonemapModuleInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        tonemapModuleInfo.codeSize = tonemapSpirv.size() * sizeof(uint32_t);
        tonemapModuleInfo.pCode = tonemapSpirv.data();
        VkShaderModule tonemapModule;
        if (vkCreateShaderModule(device.handle(), &tonemapModuleInfo, nullptr, &tonemapModule) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: tonemap shader module creation failed");
        VkComputePipelineCreateInfo tonemapPipeline {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        tonemapPipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        tonemapPipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        tonemapPipeline.stage.module = tonemapModule;
        tonemapPipeline.stage.pName = "main";
        tonemapPipeline.layout = _tonemapPipelineLayout;
        const auto tonemapResult = vkCreateComputePipelines(device.handle(), VK_NULL_HANDLE, 1,
                                                            &tonemapPipeline, nullptr, &_tonemapPipeline);
        vkDestroyShaderModule(device.handle(), tonemapModule, nullptr);
        if (tonemapResult != VK_SUCCESS)
            throw std::runtime_error("Vulkan: tonemap pipeline creation failed");

        try {
            _fsr = std::make_unique<graphics::FsrUpscaler>(device, _extent);
            _fsr->init();
        } catch (const std::exception &e) {
            // Losing the upscaler must not lose the frame; it costs the
            // anti-aliasing, since FSR is the only temporal resolve left.
            warn(std::string("FSR unavailable, rendering without anti-aliasing: ") + e.what());
            _fsr.reset();
        }
    }
#endif
    _inited = true;
}

void RayQueryPipeline::clearFrame(Frame &frame) {
    // The merge and acceleration-structure buffers are capacity-managed. The
    // renderer has waited this in-flight frame's fence before reuse, so a full
    // BLAS/TLAS rebuild may overwrite them, but their allocations survive it.
    frame.instances.reset();
    frame.traceStats.reset();
}

bool RayQueryPipeline::bakeSkyRoom(VkCommandBuffer cmd,
                                   const ModelSceneNode &room,
                                   const glm::vec3 &origin) {
    // A failed bake is deliberately sticky for this detected room: geometry is
    // the safe fallback, and retrying a known-invalid asset every frame would
    // turn that safety path into a standing cost.
    if (_skyCubeRoom == &room) {
        return _skyCubeReady;
    }
    _skyCubeRoom = &room;
    _skyCubeReady = false;

    std::vector<const RegisteredMesh *> skyMeshes;
    for (const auto &object : _gpuScene.objects()) {
        const auto *mesh = std::get_if<RegisteredMesh>(&object);
        if (!mesh || mesh->cullRoot != &room || !_gpuScene.isObjectEnabled(mesh->id.index))
            continue;
        if ((mesh->categories & (renderCategory(RenderCategory::Opaque) |
                                 renderCategory(RenderCategory::Transparent))) == 0) {
            continue;
        }
        // The bake is a fixed, module-load snapshot. Do not silently freeze a
        // deforming room or substitute a missing texture for its backdrop.
        if (!std::holds_alternative<std::monostate>(mesh->deformation))
            return false;
        const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)];
        if (!texture || !VulkanResources::supported(texture->pixelFormat()))
            return false;
        skyMeshes.push_back(mesh);
    }
    if (skyMeshes.empty())
        return false;

    auto &device = _renderer.device();
    auto &resources = _renderer.resources();
    auto &ring = _renderer.uniformRing();
    auto &descriptors = _renderer.descriptors();
    const bool hadCube = _skyCube != nullptr;
    if (!_skyCube) {
        _skyCube = std::make_unique<VulkanImage>(device);
        _skyCube->initCubeArrayAttachment({kSkyCubeSize, kSkyCubeSize}, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1);
        _skyCube->setSampler(
            resources.samplers().get(getTextureProperties(TextureUsage::ColorBuffer)));
        device.setObjectName(VK_OBJECT_TYPE_IMAGE,
                             reinterpret_cast<uint64_t>(_skyCube->handle()), "Path-traced sky cube");
    }
    bool createDepth = !_skyDepth[0];
    if (createDepth) {
        for (auto &depth : _skyDepth) {
            depth = std::make_unique<VulkanImage>(device);
            depth->initDepth({kSkyCubeSize, kSkyCubeSize}, VK_FORMAT_D32_SFLOAT);
        }
    }

    auto transition = [&](VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                          VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.image = _skyCube->handle();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = kNumCubeFaces;
        VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };
    transition(hadCube ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               hadCube ? VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
               hadCube ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (createDepth) {
        std::array<VkImageMemoryBarrier2, kNumCubeFaces> depthBarriers {};
        for (int face = 0; face < kNumCubeFaces; ++face) {
            auto &barrier = depthBarriers[face];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            barrier.image = _skyDepth[face]->handle();
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;
        }
        VkDependencyInfo dependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(depthBarriers.size());
        dependency.pImageMemoryBarriers = depthBarriers.data();
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    static const glm::vec3 kDirections[kNumCubeFaces] {
        {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
    };
    static const glm::vec3 kUps[kNumCubeFaces] {
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
    };
    const glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10000.0f);
    const VkViewport viewport {0.0f, 0.0f, static_cast<float>(kSkyCubeSize), static_cast<float>(kSkyCubeSize), 0.0f, 1.0f};
    const VkRect2D scissor {{0, 0}, {kSkyCubeSize, kSkyCubeSize}};
    const auto uniformSet = _renderer.uniformSet();

    for (int face = 0; face < kNumCubeFaces; ++face) {
        VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = _skyCube->faceRenderView(0, face);
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingAttachmentInfo depthAttachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = _skyDepth[face]->view();
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {kSkyCubeSize, kSkyCubeSize};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        rendering.pDepthAttachment = &depthAttachment;

        GlobalUniforms globals;
        globals.reset();
        globals.projection = projection;
        globals.projectionInv = glm::inverse(projection);
        globals.view = glm::lookAt(origin, origin + kDirections[face], kUps[face]);
        globals.viewInv = glm::inverse(globals.view);
        globals.viewProjection = globals.projection * globals.view;
        globals.prevViewProjection = globals.viewProjection;
        globals.cameraPosition = glm::vec4(origin, 1.0f);
        std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = ring.push(globals);

        vkCmdBeginRendering(cmd, &rendering);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        for (const auto *mesh : skyMeshes) {
            const auto &vkMesh = resources.get(mesh->mesh.get());
            VulkanPipelineCache::Key key;
            key.module = "pbr_model";
            key.vertexEntry = "staticVertex";
            key.fragmentEntry = "skyBakeFragment";
            key.colorFormats = {_skyCube->format()};
            key.depthFormat = VK_FORMAT_D32_SFLOAT;
            key.depthTest = true;
            key.depthWrite = true;
            key.cull = FaceCullMode::None;
            key.vertexBindings = VulkanMesh::bindingDescriptions(mesh->mesh.get().vertexLayout());
            key.vertexAttributes = VulkanMesh::attributeDescriptions(mesh->mesh.get().vertexLayout());
            auto &pipeline = _renderer.pipelines().get(key);

            LocalUniforms locals;
            locals.reset();
            locals.model = mesh->transform;
            locals.modelInv = mesh->transformInv;
            locals.prevModel = mesh->prevTransform;
            locals.uv = mesh->material.uv;
            offsets[UniformBlockBindingPoints::locals] = ring.push(locals);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                    VulkanDescriptors::kUniformSet, 1, &uniformSet,
                                    static_cast<uint32_t>(offsets.size()), offsets.data());
            const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)];
            auto textureSet = descriptors.acquireTextureSet(
                _renderer.frameIndex(), {{TextureUnits::mainTex, &resources.get(*texture)}});
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(),
                                    VulkanDescriptors::kTextureSet, 1, &textureSet, 0, nullptr);
            vkMesh.draw(cmd, resources.zeroBuffer());
        }
        vkCmdEndRendering(cmd);
    }

    transition(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    _skyCubeReady = true;
    info("Vulkan: baked sky room '" + room.model().name() + "' into a " + std::to_string(kSkyCubeSize) + "px cubemap",
         LogChannel::Graphics);
    return true;
}

void RayQueryPipeline::deinit() {
    for (auto &frame : _frames) {
        clearFrame(frame);
        if (frame.blas)
            vkDestroyAccelerationStructureKHR(_renderer.device().handle(), frame.blas, nullptr);
        if (frame.tlas)
            vkDestroyAccelerationStructureKHR(_renderer.device().handle(), frame.tlas, nullptr);
        frame.blas = VK_NULL_HANDLE;
        frame.tlas = VK_NULL_HANDLE;
        frame.blasStorage.reset();
        frame.tlasStorage.reset();
        frame.scratch.reset();
        frame.blasStorageCapacity = 0;
        frame.tlasStorageCapacity = 0;
        frame.scratchCapacity = 0;
    }
#ifdef R_ENABLE_NRD
    if (_compositePipeline)
        vkDestroyPipeline(_renderer.device().handle(), _compositePipeline, nullptr);
    if (_compositePipelineLayout)
        vkDestroyPipelineLayout(_renderer.device().handle(), _compositePipelineLayout, nullptr);
    if (_compositePool)
        vkDestroyDescriptorPool(_renderer.device().handle(), _compositePool, nullptr);
    if (_compositeLayout)
        vkDestroyDescriptorSetLayout(_renderer.device().handle(), _compositeLayout, nullptr);
    _compositePipeline = VK_NULL_HANDLE;
    _compositePipelineLayout = VK_NULL_HANDLE;
    _compositePool = VK_NULL_HANDLE;
    _compositeLayout = VK_NULL_HANDLE;
    _compositeSets = {};
    _temporalHistoryValid = false;
    _nrdDenoiser.reset();
    if (_nrdInstance) {
        nrd::DestroyInstance(*static_cast<nrd::Instance *>(_nrdInstance));
        _nrdInstance = nullptr;
    }
#endif
    auto &device = _renderer.device();
    _gpuScene.deinit();
    if (_pipeline)
        vkDestroyPipeline(device.handle(), _pipeline, nullptr);
    _raygenSbt.reset();
    if (_pipelineLayout)
        vkDestroyPipelineLayout(device.handle(), _pipelineLayout, nullptr);
    if (_pool)
        vkDestroyDescriptorPool(device.handle(), _pool, nullptr);
    if (_layout)
        vkDestroyDescriptorSetLayout(device.handle(), _layout, nullptr);
    if (_auxPool)
        vkDestroyDescriptorPool(device.handle(), _auxPool, nullptr);
    if (_auxLayout)
        vkDestroyDescriptorSetLayout(device.handle(), _auxLayout, nullptr);
    _auxPool = VK_NULL_HANDLE;
    _auxLayout = VK_NULL_HANDLE;
    _auxSets = {};
    for (auto &frame : _auxImages) {
        for (auto &image : frame)
            image.reset();
    }
    _auxImagesTransitioned = false;
    _pipeline = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _pool = VK_NULL_HANDLE;
    _layout = VK_NULL_HANDLE;
    _raygenSbtRegion = {};
#ifdef R_ENABLE_FSR
    // Before the device goes: the upscaler owns Vulkan objects of its own, and
    // the two images own VMA allocations that must not outlive the allocator.
    _fsr.reset();
    _fsrColor.reset();
    _fsrOutput.reset();
    _fsrImagesTransitioned = false;
    if (_tonemapPipeline)
        vkDestroyPipeline(device.handle(), _tonemapPipeline, nullptr);
    if (_tonemapPipelineLayout)
        vkDestroyPipelineLayout(device.handle(), _tonemapPipelineLayout, nullptr);
    if (_tonemapPool)
        vkDestroyDescriptorPool(device.handle(), _tonemapPool, nullptr);
    if (_tonemapLayout)
        vkDestroyDescriptorSetLayout(device.handle(), _tonemapLayout, nullptr);
    _tonemapPipeline = VK_NULL_HANDLE;
    _tonemapPipelineLayout = VK_NULL_HANDLE;
    _tonemapPool = VK_NULL_HANDLE;
    _tonemapLayout = VK_NULL_HANDLE;
    _tonemapSets = {};
#endif
    _bindlessTextureCapacity = 0;
    _lastBindlessTextureCount = 0;
    _skyCube.reset();
    for (auto &depth : _skyDepth)
        depth.reset();
    _skyFallbackCube.reset();
    _skyCubeRoom = nullptr;
    _skyCubeReady = false;
    _lastAuxFrame = -1;
    _inited = false;
}

void RayQueryPipeline::restartTemporalHistory() {
    _restartHistoryRequested = true;
#ifdef R_ENABLE_NRD
    _temporalHistoryValid = false;
#endif
}

std::vector<RayQueryPipeline::Channel> RayQueryPipeline::channels() {
    if (!_inited || _lastAuxFrame < 0) {
        return {};
    }
    // Order and names follow the aux bindings in tracing/outputs.slang.
    static constexpr const char *kNames[kNumAuxImages] {
        "Traced diffuse", "Traced specular", "Traced normal/roughness",
        "Traced viewZ", "Traced motion", "Traced noise-free", "Traced diffuse factor",
        "Traced device depth", "Traced screen motion", "Traced specular factor",
        "G-buffer diffuse", "G-buffer eye normal", "G-buffer depth", "G-buffer motion"};
    static constexpr const char *kDumpNames[kNumAuxImages] {
        "traced_diffuse", "traced_specular", "traced_normal_roughness",
        "traced_view_z", "traced_motion", "traced_noise_free", "traced_diff_factor",
        "traced_device_depth", "traced_screen_motion", "traced_spec_factor",
        "g_buffer_diffuse", "g_buffer_eye_normal", "g_buffer_depth", "g_buffer_motion"};
    const auto &aux = _auxImages[_lastAuxFrame];
    std::vector<Channel> result;
    for (int i = 0; i < kNumAuxImages; ++i) {
        if (aux[i]) {
            result.push_back({kNames[i], kDumpNames[i], aux[i].get()});
        }
    }
#ifdef R_ENABLE_NRD
    // What NRD made of the two radiance channels. Comparing these against the
    // raw pair above is the difference between "the tracer is noisy" and "the
    // denoiser is not removing it".
    if (_nrdDenoiser) {
        result.push_back({"Denoised diffuse", "denoised_diffuse", &_nrdDenoiser->denoisedDiffuse()});
        result.push_back({"Denoised specular", "denoised_specular", &_nrdDenoiser->denoisedSpecular()});
    }
#endif
    return result;
}

std::optional<GpuScene::Classification> RayQueryPipeline::classifyMesh(const RegisteredMesh &registeredMesh,
                                                                  const ModelSceneNode *skyRoom,
                                                                  bool skyBaked) {
    const auto *mesh = &registeredMesh;
    // The cubemap is ready only after the room's complete textured raster
    // bake. Until then preserve the old geometry path exactly, including
    // its candidate rejection for shadow rays.
    if (skyBaked && mesh->cullRoot == skyRoom) {
        ++_lastSky;
        return std::nullopt;
    }
    const bool saber = std::holds_alternative<RegisteredSaber>(mesh->deformation);
    const bool dangly = std::holds_alternative<RegisteredDangly>(mesh->deformation);
    const auto *skinned = std::get_if<RegisteredSkin>(&mesh->deformation);
    if (!std::holds_alternative<std::monostate>(mesh->deformation) && !skinned && !saber && !dangly) {
        ++_lastDeforming;
        return std::nullopt;
    }
    if (saber)
        ++_lastSabers;
    if (dangly)
        ++_lastDangly;
    if (mesh->id.index > 0x00ffffffu) {
        ++_lastOutOfRange;
        return std::nullopt;
    }
    if (skinned) {
        ++_lastSkinned;
    }
    InstanceMaterial material;
    material.selfIllumColor = glm::vec4(mesh->material.selfIllumColor, 0.0f);
    material.diffuseColor = glm::vec4(mesh->material.diffuseColor, 1.0f);
    material.uv0 = mesh->material.uv[0];
    material.uv1 = mesh->material.uv[1];
    material.uv2 = mesh->material.uv[2];
    material.featureMask = static_cast<uint32_t>(materialFeatureMask(mesh->material));
    // Bits 27-30 carry the object category - a scene::ModelUsage value,
    // 8 for meshes without a model root - so per-category material
    // overrides resolve at hit time without touching the layout. Must
    // match kTraceCategoryShift/Mask in slang/rayquery.slang.
    uint32_t categoryIndex = mesh->cullRoot
                                 ? static_cast<uint32_t>(mesh->cullRoot->usage())
                                 : 8u;
    material.featureMask |= (categoryIndex & 0xFu) << 27;
    // The one sky room, detected by the scene-overlap pre-pass above.
    // Its meshes render their texture as prelit radiance, terminate
    // paths, take the sky dial, and stay transparent to shadow rays in
    // the candidate shader: the environment never occludes the sun.
    // Everything else - lit panels, backdrop strips, vista rooms - is
    // plain geometry with real occlusion. The bit is tracing-local,
    // deliberately above the shared UniformsFeatureFlags range; must
    // match kTraceSky in slang/rayquery.slang.
    if (skyRoom && mesh->cullRoot == skyRoom) {
        material.featureMask |= 1u << 24;
        ++_lastSky;
    }
    // The curated per-name record - the manual level pass. Prelit is
    // Odyssey's actual selfIllum semantics: fullbright authored
    // texture, occluding, casting nothing. None strips a wrong
    // selfIllum outright. Material operations ride the same record.
    const auto *curated = _gpuScene.traceMaterials().curatedByIndex(mesh->material.curatedIndex);
    // Dangly selfIllum is Odyssey's fullbright trick for foliage, not
    // emission - danm14ab carries 653 dangly canopies that were glowing
    // and casting. Stripped by default; the curated emissive class
    // restores it for any plant that genuinely glows.
    if (dangly && (!curated || curated->klass != TraceClass::Emissive)) {
        material.selfIllumColor = glm::vec4(0.0f);
    }
    if (curated) {
        if (curated->klass == TraceClass::None) {
            material.selfIllumColor = glm::vec4(0.0f);
        }
        material.curatedAlbedoMul = glm::vec4(curated->albedoMul, 0.0f);
        material.curatedRoughA = glm::vec4(static_cast<float>(curated->roughnessMode),
                                           curated->roughnessParams.x,
                                           curated->roughnessParams.y,
                                           curated->roughnessParams.z);
        material.curatedRoughB = glm::vec4(curated->roughnessParams.w,
                                           curated->roughnessWeights.x,
                                           curated->roughnessWeights.y,
                                           curated->roughnessWeights.z);
        material.curatedMetalA = glm::vec4(static_cast<float>(curated->metallicMode),
                                           curated->metallicParams.x,
                                           curated->metallicParams.y,
                                           curated->metallicParams.z);
        material.curatedMetalB = glm::vec4(curated->metallicParams.w,
                                           curated->metallicWeights.x,
                                           curated->metallicWeights.y,
                                           curated->metallicWeights.z);
        material.curatedEmission = glm::vec4(curated->emissionValue,
                                             static_cast<float>(curated->emissionMode));
    }
    // The surface model, assigned here - the shader never classifies.
    // Additive-blended diffuse is Odyssey's other authored glow: no
    // selfIllum controller, the texture is the light, and the ray passes
    // through it (saber blades, glow decals). The sky room and curated
    // prelit imagery are unlit emissive: radiance as authored, path
    // ends. Everything else is PBR; transparent non-additive meshes -
    // alpha-blended leaves above all - carry their coverage in diffuse
    // alpha and resolve stochastically in the surface model.
    if (const auto *diffuse = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        if (diffuse->features().blending == Texture::Blending::Additive) {
            material.surfaceType = 1;
            ++_lastAdditive;
        } else if (diffuse->features().blending == Texture::Blending::PunchThrough ||
                   mesh->material.type == MaterialType::TransparentModel) {
            material.featureMask |= 1u << 26;
        }
    }
    if ((material.featureMask & (1u << 24)) != 0 ||
        (curated && curated->klass == TraceClass::Prelit)) {
        material.surfaceType = 2;
    }
    // The per-category calibration override, baked per instance so the
    // dials stay live through the per-frame admission - no GPU table.
    {
        const auto &src = _options.ptCategoryOverrides[std::min<uint32_t>(categoryIndex, 8u)];
        material.overrideColor = glm::vec4(src.color[0], src.color[1], src.color[2],
                                           std::clamp(src.colorWeight, 0.0f, 1.0f));
        material.overrideParams = glm::vec4(src.roughness,
                                            std::max(0.0f, src.emissionScale),
                                            std::max(0.0f, src.envScale),
                                            std::max(0.0f, src.metallicScale));
        material.roughnessScale = std::max(0.0f, src.roughnessScale);
        // Baked into the emission values here rather than left for the
        // shader to multiply. Emission reaches the surface models by two
        // routes - the self-illum controller and the curated override -
        // and only one of them passed through a scale, so the dial moved
        // some emitters and not others.
        // Additive surfaces are the exception: their emission is
        // max(selfIllum, 1) * albedo, so a scale folded into selfIllum
        // vanishes below 1 and would double up above it. That model keeps
        // reading overrideParams.y directly.
        const float emissionScale = std::max(0.0f, src.emissionScale);
        if (material.surfaceType != 1) {
            material.selfIllumColor *= emissionScale;
        }
        if (curated && curated->emissionMode != 0) {
            material.curatedEmission = glm::vec4(glm::vec3(material.curatedEmission) * emissionScale,
                                                 material.curatedEmission.w);
        }
    }
    if (const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::NormalMap)]) {
        material.normalMap = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)]) {
        material.lightmap = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::BumpMapArray)]) {
        material.bumpMapArray = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
        material.bumpMapFrame = mesh->material.bumpMapFrame;
        material.bumpMapScale = texture->features().bumpMapScaling;
    }
    const bool nonOpaque = material.surfaceType == 1 ||
                           (material.featureMask & ((1u << 24) | (1u << 26))) != 0;
    _lastDynamicTriangles += (skinned || dangly || saber) ? static_cast<uint32_t>(mesh->mesh.get().faces().size()) : 0;
    if (!dangly && (!curated || curated->klass == TraceClass::Default) &&
        glm::any(glm::greaterThan(mesh->material.selfIllumColor, glm::vec3(0.0f))))
        ++_lastEmissive;
    // Material::staticObject is an authored room hint, not the admission
    // proof required to retain geometry. Until a stronger classifier exists,
    // publish this consumer's objects as dynamic.
    return {{material, nonOpaque ? GpuScene::PrimitiveClass::NonOpaque : GpuScene::PrimitiveClass::Opaque,
             GpuScene::ResidencyClass::Dynamic, skinned}};
}

std::optional<GpuScene::Classification> RayQueryPipeline::classifyGrass(const RegisteredGrass &grass) {
    // Clusters, not entries: one RegisteredGrass carries a whole hillside, so
    // counting entries reported 1 where 1482 quads were admitted. The point of
    // this counter is to answer "is it actually there", which a count of
    // registrations cannot.
    _lastGrass += static_cast<uint32_t>(grass.instances.size());
    InstanceMaterial material;
    material.diffuseColor = glm::vec4(grass.material.diffuseColor, 1.0f);
    material.uv0 = grass.material.uv[0];
    material.uv1 = grass.material.uv[1];
    material.uv2 = grass.material.uv[2];
    // Raster grass always uses hashed coverage, irrespective of the texture's
    // blending metadata. Preserve that same candidate semantics for the BLAS.
    material.featureMask = static_cast<uint32_t>(materialFeatureMask(grass.material)) |
                           UniformsFeatureFlags::hashedalphatest | (1u << 26) | (8u << 27);
    if (const auto *texture = grass.material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    if (const auto *texture = grass.material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)]) {
        material.lightmap = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    const auto &src = _options.ptCategoryOverrides[8];
    material.overrideColor = glm::vec4(src.color[0], src.color[1], src.color[2],
                                       std::clamp(src.colorWeight, 0.0f, 1.0f));
    material.overrideParams = glm::vec4(src.roughness,
                                        std::max(0.0f, src.emissionScale),
                                        std::max(0.0f, src.envScale),
                                        std::max(0.0f, src.metallicScale));
    material.roughnessScale = std::max(0.0f, src.roughnessScale);
    return {{material, GpuScene::PrimitiveClass::NonOpaque,
             GpuScene::ResidencyClass::Dynamic, nullptr}};
}

std::optional<GpuScene::Classification> RayQueryPipeline::classifyParticles(const RegisteredParticles &particles) {
    _lastParticles += static_cast<uint32_t>(particles.instances.size());
    InstanceMaterial material;
    material.diffuseColor = glm::vec4(particles.material.diffuseColor, 1.0f);
    material.uv0 = particles.material.uv[0];
    material.uv1 = particles.material.uv[1];
    material.uv2 = particles.material.uv[2];
    material.featureMask = static_cast<uint32_t>(materialFeatureMask(particles.material));
    if (const auto *texture = particles.material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
    }
    // Particle OIT is a coverage layer. The tracer has no OIT compositing
    // path, so ordinary particles use its non-opaque alpha candidate path;
    // additive emitters retain their existing pass-through emission semantics.
    if (particles.material.blending == BlendMode::Lighten) {
        material.surfaceType = 1;
    } else {
        // Particle OIT represents continuous coverage. It is a real PBR
        // surface in the tracer, with the unoccluded portion transmitted.
        // Without this bit ptTraceNearest commits the quad unconditionally
        // and never reads its alpha at all, which renders smoke as solid
        // black blocks.
        material.featureMask |= 1u << 25;
    }
    const auto &src = _options.ptCategoryOverrides[8];
    material.overrideColor = glm::vec4(src.color[0], src.color[1], src.color[2],
                                       std::clamp(src.colorWeight, 0.0f, 1.0f));
    material.overrideParams = glm::vec4(src.roughness,
                                        std::max(0.0f, src.emissionScale),
                                        std::max(0.0f, src.envScale),
                                        std::max(0.0f, src.metallicScale));
    material.roughnessScale = std::max(0.0f, src.roughnessScale);
    return {{material, GpuScene::PrimitiveClass::NonOpaque,
             GpuScene::ResidencyClass::Dynamic, nullptr}};
}

std::optional<GpuScene::Classification> RayQueryPipeline::classifyBillboard(const RegisteredBillboard &billboard) {
    ++_lastBillboards;
    InstanceMaterial material;
    material.diffuseColor = billboard.color;
    material.mainTex = _renderer.resources().textureId(billboard.texture.get()).value_or(UINT32_MAX);
    // Registered billboards are lens flares: raster draws them additively with
    // no depth test. They are nevertheless represented in the BLAS so rays
    // can see their emitted contribution, while remaining non-occluding.
    material.surfaceType = 1;
    return {{material, GpuScene::PrimitiveClass::NonOpaque,
             GpuScene::ResidencyClass::Dynamic, nullptr}};
}

void RayQueryPipeline::render(VkCommandBuffer cmd, uint32_t globalsOffset,
                              VulkanImage &output,
                              const glm::mat4 &view, const glm::mat4 &projection,
                              const glm::vec4 &jitter) {
    // This intentionally bypasses drawScene: its frustum/distance policy must
    // not decide what a ray can hit. Keep the explicit policy construction as
    // the documented caller of the no-culling mode.
    const auto visibility = VisibilityPolicy::noCulling();
    (void)visibility;
    const int frameIndex = _renderer.frameIndex();
    // A valid traced frame can contain no merged geometry. That path clears
    // the output and returns below, but its auxiliary images are still useful
    // diagnostics (and must not disappear from --dumptargets just because the
    // scene is empty). Record the selected double-buffer slot before that
    // early return, rather than only after the trace dispatch.
    _lastAuxFrame = frameIndex;
    auto &frame = _frames[frameIndex];
    // The renderer waited this in-flight frame's fence before calling us, so
    // its previous GPU-written counters are now safe to inspect and every
    // frame-local skinned BLAS/output buffer may be retired.
    if (frame.traceStats) {
        frame.traceStats->invalidateMapped();
        const auto *stats = static_cast<const TraceStats *>(frame.traceStats->mapped());
        _lastSecondaryRays = stats->secondaryRays;
        _lastSecondaryMisses = stats->secondaryMisses;
        _lastSurvivingLights = stats->survivingLights;
        _lastPrimaryHits = stats->primaryHits;
        _lastShadowRays = stats->shadowRays;
    }
    clearFrame(frame);
    _lastDeforming = 0;
    _lastSkinned = 0;
    _lastTriangles = 0;
    _lastDynamicTriangles = 0;
    _lastOutOfRange = 0;
    _lastEmissive = 0;
    _lastAdditive = 0;
    _lastSabers = 0;
    _lastSky = 0;
    _lastDangly = 0;
    _lastGrass = 0;
    _lastParticles = 0;
    _lastBillboards = 0;
    // Sky detection: there is exactly ONE sky per scene - the room whose
    // geometry overlaps the scene itself. Candidates are rooms without a
    // walkmesh (background scenery, the K1 skybox convention - a walkable
    // room can never be the sky, so a one-room interior never qualifies);
    // among them, the room whose union bounds cover the scene on all three
    // axes above a relative threshold wins, and every mesh of that room
    // classifies. Modules stitch the dome from several pieces of one sky
    // room, which is why the test is per room, not per mesh. Semantic and
    // geometric, never color-based.
    static constexpr float kSkyOverlapThreshold = 0.5f;
    glm::vec3 sceneMin(std::numeric_limits<float>::max());
    glm::vec3 sceneMax(std::numeric_limits<float>::lowest());
    struct SkyRoomCandidate {
        glm::vec3 boundsMin {std::numeric_limits<float>::max()};
        glm::vec3 boundsMax {std::numeric_limits<float>::lowest()};
    };
    std::map<const ModelSceneNode *, SkyRoomCandidate> sceneryRooms;
    for (const auto &object : _gpuScene.objects()) {
        const auto *mesh = std::get_if<RegisteredMesh>(&object);
        if (!mesh || !mesh->cullRoot || mesh->cullRoot->usage() != ModelUsage::Room)
            continue;
        if (!_gpuScene.isObjectEnabled(mesh->id.index))
            continue;
        if ((mesh->categories & (renderCategory(RenderCategory::Opaque) |
                                 renderCategory(RenderCategory::Transparent))) == 0) {
            continue;
        }
        auto worldAabb = mesh->mesh.get().aabb() * mesh->transform;
        sceneMin = glm::min(sceneMin, worldAabb.min());
        sceneMax = glm::max(sceneMax, worldAabb.max());
        if (mesh->cullRoot->isBackgroundScenery()) {
            auto &candidate = sceneryRooms[mesh->cullRoot];
            candidate.boundsMin = glm::min(candidate.boundsMin, worldAabb.min());
            candidate.boundsMax = glm::max(candidate.boundsMax, worldAabb.max());
        }
    }
    const ModelSceneNode *skyRoom = nullptr;
    glm::vec3 skyOrigin(0.0f);
    if (!sceneryRooms.empty()) {
        glm::vec3 sceneExtent = glm::max(sceneMax - sceneMin, glm::vec3(1e-3f));
        float bestVolume = 0.0f;
        for (const auto &[room, candidate] : sceneryRooms) {
            glm::vec3 extent = candidate.boundsMax - candidate.boundsMin;
            glm::vec3 ratios = extent / sceneExtent;
            if (glm::min(ratios.x, glm::min(ratios.y, ratios.z)) < kSkyOverlapThreshold) {
                continue;
            }
            float volume = extent.x * extent.y * extent.z;
            if (volume > bestVolume) {
                bestVolume = volume;
                skyRoom = room;
                skyOrigin = 0.5f * (candidate.boundsMin + candidate.boundsMax);
            }
        }
        if (skyRoom && _frameNumber == 0) {
            info("Vulkan: sky room is '" + skyRoom->model().name() + "'", LogChannel::Graphics);
        }
    }
    // Published for the Objects panel's classification column: the panel
    // reports the actual decision, not a re-derivation of it.
    _gpuScene.setSkyRoom(skyRoom);
    bool skyBaked = false;
    if (skyRoom) {
        try {
            skyBaked = bakeSkyRoom(cmd, *skyRoom, skyOrigin);
        } catch (const std::exception &e) {
            // Never trade a heuristic misfire or a broken bake for removed
            // scene geometry. Keep the room in the merged BLAS this frame.
            _skyCubeReady = false;
            warn("Vulkan: sky bake failed for '" + skyRoom->model().name() + "': " + e.what() +
                     "; keeping sky geometry",
                 LogChannel::Graphics);
        }
    } else {
        _skyCubeRoom = nullptr;
        _skyCubeReady = false;
    }
    const auto scene = _gpuScene.update(
        cmd,
        [this, skyRoom, skyBaked](const RegisteredMesh &mesh) {
            return classifyMesh(mesh, skyRoom, skyBaked);
        },
        [this](const RegisteredGrass &grass) { return classifyGrass(grass); },
        [this](const RegisteredParticles &particles) { return classifyParticles(particles); },
        [this](const RegisteredBillboard &billboard) { return classifyBillboard(billboard); },
        view);
    if (!scene.vertices.buffer) {
        VkClearColorValue clear {{0.02f, 0.03f, 0.06f, 1.0f}};
        VkImageSubresourceRange range {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(cmd, output.handle(), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        return;
    }
    _lastInstances = 1;
    _lastTriangles = scene.triangleCount;
    auto &device = _renderer.device();
    frame.traceStats = std::make_unique<VulkanBuffer>(device);
    frame.traceStats->initHostVisibleReadback(sizeof(TraceStats), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(frame.traceStats->mapped(), 0, sizeof(TraceStats));

    const VkDeviceAddress geometryAddress = scene.vertices.buffer->deviceAddress() + scene.vertices.offset;
    std::array<VkAccelerationStructureGeometryTrianglesDataKHR, 2> triangleData {};
    std::array<VkAccelerationStructureGeometryKHR, 2> blasGeometries {};
    for (auto &triangles : triangleData) {
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = geometryAddress;
        triangles.vertexStride = sizeof(GpuScene::MergedVertex);
        triangles.maxVertex = scene.vertexCount - 1;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = scene.indices.buffer->deviceAddress() + scene.indices.offset;
    }
    for (uint32_t i = 0; i < blasGeometries.size(); ++i) {
        auto &geometry = blasGeometries[i];
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.geometry.triangles = triangleData[i];
    }
    // Geometry 1 deliberately remains non-opaque: the ray-query candidate
    // loop evaluates its additive, alpha-tested, and sky surfaces itself.
    blasGeometries[0].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    blasGeometries[1].flags = 0;
    VkAccelerationStructureBuildGeometryInfoKHR blasBuild {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    blasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasBuild.geometryCount = static_cast<uint32_t>(blasGeometries.size());
    blasBuild.pGeometries = blasGeometries.data();
    const std::array<uint32_t, 2> blasPrimitiveCounts {{
        scene.opaqueTriangleCount,
        scene.triangleCount - scene.opaqueTriangleCount,
    }};
    VkAccelerationStructureBuildSizesInfoKHR blasSizes {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &blasBuild, blasPrimitiveCounts.data(), &blasSizes);
    if (!frame.blas || blasSizes.accelerationStructureSize > frame.blasStorageCapacity) {
        if (frame.blas)
            vkDestroyAccelerationStructureKHR(device.handle(), frame.blas, nullptr);
        frame.blas = VK_NULL_HANDLE;
        frame.blasStorage.reset();
        frame.blasStorageCapacity = grownCapacity(frame.blasStorageCapacity,
                                                  blasSizes.accelerationStructureSize, 64 * 1024);
        frame.blasStorage = std::make_unique<VulkanBuffer>(device);
        frame.blasStorage->initDeviceLocal(frame.blasStorageCapacity,
                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                           nullptr);
        VkAccelerationStructureCreateInfoKHR create {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer = frame.blasStorage->handle();
        create.size = frame.blasStorageCapacity;
        create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(device.handle(), &create, nullptr, &frame.blas) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: merged BLAS creation failed");
    }

    VkAccelerationStructureDeviceAddressInfoKHR blasAddressInfo {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    blasAddressInfo.accelerationStructure = frame.blas;
    VkAccelerationStructureInstanceKHR instance {};
    instance.transform = instanceTransform(glm::mat4(1.0f));
    instance.instanceCustomIndex = 0;
    instance.mask = 0xff;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference =
        vkGetAccelerationStructureDeviceAddressKHR(device.handle(), &blasAddressInfo);
    frame.instances = std::make_unique<VulkanBuffer>(device);
    frame.instances->initHostVisible(sizeof(instance), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    std::memcpy(frame.instances->mapped(), &instance, sizeof(instance));

    VkAccelerationStructureGeometryInstancesDataKHR instanceData {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instanceData.arrayOfPointers = VK_FALSE;
    instanceData.data.deviceAddress = frame.instances->deviceAddress();
    VkAccelerationStructureGeometryKHR tlasGeometry {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances = instanceData;
    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuild.geometryCount = 1;
    tlasBuild.pGeometries = &tlasGeometry;
    constexpr uint32_t kTlasInstanceCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &tlasBuild, &kTlasInstanceCount, &tlasSizes);
    if (!frame.tlas || tlasSizes.accelerationStructureSize > frame.tlasStorageCapacity) {
        if (frame.tlas)
            vkDestroyAccelerationStructureKHR(device.handle(), frame.tlas, nullptr);
        frame.tlas = VK_NULL_HANDLE;
        frame.tlasStorage.reset();
        frame.tlasStorageCapacity = grownCapacity(frame.tlasStorageCapacity,
                                                  tlasSizes.accelerationStructureSize, 64 * 1024);
        frame.tlasStorage = std::make_unique<VulkanBuffer>(device);
        frame.tlasStorage->initDeviceLocal(frame.tlasStorageCapacity,
                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                           nullptr);
        VkAccelerationStructureCreateInfoKHR create {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer = frame.tlasStorage->handle();
        create.size = frame.tlasStorageCapacity;
        create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(device.handle(), &create, nullptr, &frame.tlas) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: TLAS creation failed");
    }

    const auto alignment = device.accelerationStructureProperties().minAccelerationStructureScratchOffsetAlignment;
    const VkDeviceSize scratchSize = std::max(blasSizes.buildScratchSize, tlasSizes.buildScratchSize);
    if (scratchSize > std::numeric_limits<VkDeviceSize>::max() - alignment) {
        throw std::runtime_error("Vulkan: acceleration-structure scratch size exceeds device-size range");
    }
    const VkDeviceSize scratchAllocationSize = scratchSize + alignment;
    if (!frame.scratch || scratchAllocationSize > frame.scratchCapacity) {
        frame.scratch.reset();
        frame.scratchCapacity = grownCapacity(frame.scratchCapacity, scratchAllocationSize, 64 * 1024);
        frame.scratch = std::make_unique<VulkanBuffer>(device);
        frame.scratch->initDeviceLocal(frame.scratchCapacity,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr);
    }
    const VkDeviceAddress scratchAddress = alignedAddress(frame.scratch->deviceAddress(), alignment);
    blasBuild.dstAccelerationStructure = frame.blas;
    blasBuild.scratchData.deviceAddress = scratchAddress;
    std::array<VkAccelerationStructureBuildRangeInfoKHR, 2> blasRanges {};
    blasRanges[0].primitiveCount = blasPrimitiveCounts[0];
    blasRanges[1].primitiveCount = blasPrimitiveCounts[1];
    blasRanges[1].primitiveOffset = static_cast<uint32_t>(scene.opaqueTriangleCount * 3 * sizeof(uint32_t));
    const VkAccelerationStructureBuildRangeInfoKHR *blasRangePointers[] {
        &blasRanges[0], &blasRanges[1]};
    const auto begin = std::chrono::steady_clock::now();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &blasBuild, blasRangePointers);
    // The TLAS build reads the merged BLAS, so keep this build-to-build
    // dependency separate from the later build-to-trace hand-off.
    VkMemoryBarrier2 blasToTlas {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    blasToTlas.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    blasToTlas.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    blasToTlas.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    blasToTlas.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo blasToTlasDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    blasToTlasDependency.memoryBarrierCount = 1;
    blasToTlasDependency.pMemoryBarriers = &blasToTlas;
    vkCmdPipelineBarrier2(cmd, &blasToTlasDependency);
    tlasBuild.dstAccelerationStructure = frame.tlas;
    tlasBuild.scratchData.deviceAddress = scratchAddress;
    VkAccelerationStructureBuildRangeInfoKHR tlasRange {};
    tlasRange.primitiveCount = kTlasInstanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR *tlasRanges[] {&tlasRange};
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasBuild, tlasRanges);
    VkMemoryBarrier2 tlasToTrace {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    tlasToTrace.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    tlasToTrace.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    tlasToTrace.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    tlasToTrace.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo tlasToTraceDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    tlasToTraceDependency.memoryBarrierCount = 1;
    tlasToTraceDependency.pMemoryBarriers = &tlasToTrace;
    vkCmdPipelineBarrier2(cmd, &tlasToTraceDependency);

    VkDescriptorImageInfo image {};
    image.imageView = output.view();
    image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSetAccelerationStructureKHR asWrite {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &frame.tlas;
    VkDescriptorBufferInfo materialBuffer {};
    materialBuffer.buffer = scene.materials.buffer->handle();
    materialBuffer.range = scene.materials.size;
    VkDescriptorBufferInfo statsBuffer {};
    statsBuffer.buffer = frame.traceStats->handle();
    statsBuffer.range = frame.traceStats->size();
    const VkDeviceSize vertexBytes = scene.vertices.size;
    const VkDeviceSize indexBytes = scene.indices.size;
    VkDescriptorBufferInfo mergedVertices {};
    mergedVertices.buffer = scene.vertices.buffer->handle();
    mergedVertices.offset = scene.vertices.offset;
    mergedVertices.range = scene.vertices.size;
    VkDescriptorBufferInfo mergedIndices {};
    mergedIndices.buffer = scene.vertices.buffer->handle();
    mergedIndices.offset = scene.indices.offset;
    mergedIndices.range = scene.indices.size;
    VkDescriptorBufferInfo mergedMaterialIds {};
    mergedMaterialIds.buffer = scene.materialIds.buffer->handle();
    mergedMaterialIds.offset = scene.materialIds.offset;
    mergedMaterialIds.range = scene.materialIds.size;
    VkWriteDescriptorSet writes[7] {};
    const auto set = _sets[_renderer.frameIndex()];
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &image;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].pNext = &asWrite;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &materialBuffer;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &statsBuffer;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = set;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &mergedVertices;
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = set;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo = &mergedIndices;
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = set;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[6].pBufferInfo = &mergedMaterialIds;
    vkUpdateDescriptorSets(device.handle(), 7, writes, 0, nullptr);
    const VulkanImage &skyImage = skyBaked ? *_skyCube : *_skyFallbackCube;
    VkDescriptorImageInfo skyInfo {
        skyImage.sampler(), skyBaked ? _skyCube->cubeView(0) : _skyFallbackCube->view(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet skyWrite {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    skyWrite.dstSet = set;
    skyWrite.dstBinding = 9;
    skyWrite.descriptorCount = 1;
    skyWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    skyWrite.pImageInfo = &skyInfo;
    vkUpdateDescriptorSets(device.handle(), 1, &skyWrite, 0, nullptr);
    // Texture ids are assigned by VulkanResources at upload time. The set is
    // update-after-bind and partially-bound so new assets can take a slot
    // without rebuilding it or populating unrelated descriptors.
    const auto uploadedTextures = _renderer.resources().uploadedTextures();
    std::vector<VkDescriptorImageInfo> textureInfos;
    std::vector<VkWriteDescriptorSet> textureWrites;
    textureInfos.reserve(uploadedTextures.size());
    textureWrites.reserve(uploadedTextures.size());
    for (const auto &[id, texture] : uploadedTextures) {
        if (id >= _bindlessTextureCapacity) {
            throw std::runtime_error("Vulkan: ray-query bindless texture array exhausted");
        }
        textureInfos.push_back({texture->sampler(), texture->view(),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        VkWriteDescriptorSet textureWrite {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        textureWrite.dstSet = set;
        textureWrite.dstBinding = 7;
        textureWrite.dstArrayElement = id;
        textureWrite.descriptorCount = 1;
        textureWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        textureWrite.pImageInfo = &textureInfos.back();
        textureWrites.push_back(textureWrite);
    }
    if (!textureWrites.empty()) {
        vkUpdateDescriptorSets(device.handle(), static_cast<uint32_t>(textureWrites.size()),
                               textureWrites.data(), 0, nullptr);
    }
    const auto uploadedTextureArrays = _renderer.resources().uploadedTextureArrays();
    textureInfos.clear();
    textureWrites.clear();
    textureInfos.reserve(uploadedTextureArrays.size());
    textureWrites.reserve(uploadedTextureArrays.size());
    for (const auto &[id, texture] : uploadedTextureArrays) {
        if (id >= _bindlessTextureCapacity) {
            throw std::runtime_error("Vulkan: ray-query bindless texture array exhausted");
        }
        textureInfos.push_back({texture->sampler(), texture->view(),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        VkWriteDescriptorSet textureWrite {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        textureWrite.dstSet = set;
        textureWrite.dstBinding = 8;
        textureWrite.dstArrayElement = id;
        textureWrite.descriptorCount = 1;
        textureWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        textureWrite.pImageInfo = &textureInfos.back();
        textureWrites.push_back(textureWrite);
    }
    if (!textureWrites.empty()) {
        vkUpdateDescriptorSets(device.handle(), static_cast<uint32_t>(textureWrites.size()),
                               textureWrites.data(), 0, nullptr);
    }
    _lastBindlessTextureCount = static_cast<uint32_t>(uploadedTextures.size());
    if (!_auxImagesTransitioned) {
        // Once, at the first traced frame: every aux image moves from
        // UNDEFINED to GENERAL, both in-flight copies at a stroke.
        std::array<VkImageMemoryBarrier2, 2 * kNumAuxImages> auxBarriers {};
        for (int frameIndex = 0; frameIndex < 2; ++frameIndex) {
            for (int i = 0; i < kNumAuxImages; ++i) {
                auto &barrier = auxBarriers[frameIndex * kNumAuxImages + i];
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
                barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.image = _auxImages[frameIndex][i]->handle();
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;
            }
        }
        VkDependencyInfo auxDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        auxDependency.imageMemoryBarrierCount = static_cast<uint32_t>(auxBarriers.size());
        auxDependency.pImageMemoryBarriers = auxBarriers.data();
        vkCmdPipelineBarrier2(cmd, &auxDependency);
        _auxImagesTransitioned = true;
    }
    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[0] = globalsOffset;
    auto uniformSet = _renderer.uniformSet();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _pipelineLayout, 0, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _pipelineLayout,
                            1, 1, &set, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, _pipelineLayout, 2, 1,
                            &_auxSets[_renderer.frameIndex()], 0, nullptr);
    // Clamped rather than trusted: the option is user-editable in reone.cfg
    // and a zero would divide the accumulated radiance by zero.
    TracePushConstants constants {_frameNumber,
                                  static_cast<uint32_t>(std::max(1, _options.pathTracingSamples)),
                                  std::max(0.0f, _options.ptSkyIntensity),
                                  std::max(0.0f, _options.ptEmissiveIntensity),
                                  std::max(0.0f, _options.ptLightmapIntensity),
                                  std::max(0.0f, _options.ptDirectIntensity),
                                  std::max(0.0001f, _options.ptRayOffset),
                                  std::max(0.0f, _options.ptSunIntensity),
                                  (_options.ptTraceStats ? 1u : 0u) |
                                      (static_cast<uint32_t>(std::clamp(_options.ptDebugView, 0, 12)) << 4) |
                                      (static_cast<uint32_t>(std::clamp(_options.ptTonemap, 0, 1)) << 8),
                                  static_cast<uint32_t>(std::clamp(_options.ptBounces, 1, 8)),
                                  std::clamp(_options.ptPointEmitterRatio, 0.01f, 0.5f),
                                  glm::radians(std::clamp(_options.ptSunAngularSize, 0.05f, 10.0f)),
                                  std::max(0.01f, _options.ptExposure),
                                  0,
                                  scene.opaqueTriangleCount,
                                  skyBaked ? 1u : 0u};
    vkCmdPushConstants(cmd, _pipelineLayout, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(constants), &constants);
    const VkStridedDeviceAddressRegionKHR emptySbt {};
    vkCmdTraceRaysKHR(cmd, &_raygenSbtRegion, &emptySbt, &emptySbt, &emptySbt,
                      static_cast<uint32_t>(_extent.x), static_cast<uint32_t>(_extent.y), 1);
#ifdef R_ENABLE_NRD
    if (_nrdDenoiser) {
        // The trace pass's storage writes feed NRD's sampled reads.
        VkMemoryBarrier2 traceToDenoise {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        traceToDenoise.srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        traceToDenoise.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        traceToDenoise.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        traceToDenoise.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo traceDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        traceDependency.memoryBarrierCount = 1;
        traceDependency.pMemoryBarriers = &traceToDenoise;
        vkCmdPipelineBarrier2(cmd, &traceDependency);
        const auto &aux = _auxImages[_renderer.frameIndex()];
        NrdDenoiser::Inputs inputs;
        inputs.diffRadianceHitDist = aux[0]->view();
        inputs.specRadianceHitDist = aux[1]->view();
        inputs.normalRoughness = aux[2]->view();
        inputs.viewZ = aux[3]->view();
        inputs.motion = aux[4]->view();
        // The projection arrives carrying the TAA jitter (applied as a clip
        // translate); NRD is owed the unjittered matrix and the sub-pixel
        // offset separately, the latter in pixels with UV-down y.
        glm::mat4 unjitteredProjection =
            glm::translate(glm::vec3(-jitter.x, -jitter.y, 0.0f)) * projection;
        glm::vec2 jitterPixels {jitter.x * 0.5f * static_cast<float>(_extent.x),
                                -jitter.y * 0.5f * static_cast<float>(_extent.y)};
        NrdDenoiser::Tuning tuning;
        tuning.maxAccumulatedFrames = _options.ptNrdMaxAccumulatedFrames;
        tuning.maxFastAccumulatedFrames = _options.ptNrdMaxFastAccumulatedFrames;
        tuning.maxStabilizedFrames = _options.ptNrdMaxStabilizedFrames;
        tuning.historyFixFrames = _options.ptNrdHistoryFixFrames;
        tuning.diffusePrepassBlurRadius = _options.ptNrdDiffusePrepassBlurRadius;
        tuning.specularPrepassBlurRadius = _options.ptNrdSpecularPrepassBlurRadius;
        tuning.minBlurRadius = _options.ptNrdMinBlurRadius;
        tuning.maxBlurRadius = _options.ptNrdMaxBlurRadius;
        tuning.lobeAngleFraction = _options.ptNrdLobeAngleFraction;
        tuning.roughnessFraction = _options.ptNrdRoughnessFraction;
        tuning.planeDistanceSensitivity = _options.ptNrdPlaneDistanceSensitivity;
        tuning.disocclusionThreshold = _options.ptNrdDisocclusionThreshold;
        tuning.antiFirefly = _options.ptNrdAntiFirefly;
        const bool restartHistory = _frameNumber == 0 || _restartHistoryRequested;
        _restartHistoryRequested = false;
        _nrdDenoiser->denoise(cmd, _renderer.frameIndex(), inputs, tuning, view, unjitteredProjection,
                              jitterPixels, _frameNumber, restartHistory);
        if (_options.ptDenoise && _options.ptDebugView == 0) {
            // The noise-free history resets whenever NRD's would: first
            // frame, or a teleport-sized camera jump.
            const auto cameraPosition = glm::vec3(glm::inverse(view)[3]);
            if (_frameNumber == 0 ||
                glm::distance(cameraPosition, _prevCameraPosition) > 20.0f) {
                _temporalHistoryValid = false;
            }
            _prevCameraPosition = cameraPosition;
            const bool temporalReset = !_temporalHistoryValid;

            // The assembly from denoised channels, overwriting the trace
            // kernel's own write. Debug views keep the kernel's output.
            // History ping-pong: the frame at index i reads what the previous
            // frame (index 1-i) wrote into slot i, and writes slot 1-i.
            const auto frameIndex = _renderer.frameIndex();
            const auto compositeSet = _compositeSets[frameIndex];
            // With FSR the composite hands off linear HDR to the upscaler
            // instead of writing the finished frame; the display transform
            // happens after, in pt_tonemap.
            bool fsrActive = false;
#ifdef R_ENABLE_FSR
            fsrActive = _fsr && _fsr->inited();
#endif
            VkImageView compositeTarget = output.view();
#ifdef R_ENABLE_FSR
            if (fsrActive) {
                if (!_fsrImagesTransitioned) {
                    std::array<VkImageMemoryBarrier2, 2> fsrBarriers {};
                    VulkanImage *fsrImages[] {_fsrColor.get(), _fsrOutput.get()};
                    for (int i = 0; i < 2; ++i) {
                        auto &barrier = fsrBarriers[i];
                        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                        barrier.image = fsrImages[i]->handle();
                        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        barrier.subresourceRange.levelCount = 1;
                        barrier.subresourceRange.layerCount = 1;
                    }
                    VkDependencyInfo fsrDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    fsrDependency.imageMemoryBarrierCount = 2;
                    fsrDependency.pImageMemoryBarriers = fsrBarriers.data();
                    vkCmdPipelineBarrier2(cmd, &fsrDependency);
                    _fsrImagesTransitioned = true;
                }
                compositeTarget = _fsrColor->view();
            }
#endif
            // Motion is not among them: it existed only for the removed TAA's
            // reprojection. NRD still consumes it directly.
            constexpr uint32_t kCompositeBindings = 7;
            std::array<VkDescriptorImageInfo, kCompositeBindings> compositeImages {{
                {VK_NULL_HANDLE, compositeTarget, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, aux[5]->view(), VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, aux[6]->view(), VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, aux[9]->view(), VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, _nrdDenoiser->denoisedDiffuse().view(), VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, _nrdDenoiser->denoisedSpecular().view(), VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, aux[3]->view(), VK_IMAGE_LAYOUT_GENERAL},
            }};
            std::array<VkWriteDescriptorSet, kCompositeBindings> compositeWrites {};
            for (uint32_t i = 0; i < kCompositeBindings; ++i) {
                compositeWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                compositeWrites[i].dstSet = compositeSet;
                compositeWrites[i].dstBinding = i;
                compositeWrites[i].descriptorCount = 1;
                compositeWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                compositeWrites[i].pImageInfo = &compositeImages[i];
            }
            vkUpdateDescriptorSets(device.handle(), kCompositeBindings, compositeWrites.data(), 0, nullptr);
            struct CompositePush {
                uint32_t tonemap;
                float exposure;
                uint32_t linearOutput;
            } compositePush {static_cast<uint32_t>(std::clamp(_options.ptTonemap, 0, 1)),
                             std::max(0.01f, _options.ptExposure),
                             fsrActive ? 1u : 0u};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _compositePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _compositePipelineLayout, 0, 1,
                                    &uniformSet, static_cast<uint32_t>(offsets.size()), offsets.data());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _compositePipelineLayout,
                                    1, 1, &compositeSet, 0, nullptr);
            vkCmdPushConstants(cmd, _compositePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(compositePush), &compositePush);
            vkCmdDispatch(cmd, static_cast<uint32_t>((_extent.x + 7) / 8),
                          static_cast<uint32_t>((_extent.y + 7) / 8), 1);
            _temporalHistoryValid = true;
#ifdef R_ENABLE_FSR
            if (fsrActive) {
                // Composite writes, FSR reads. FSR's backend barriers its own
                // internal resources but not ours, so the handoff is ours.
                VkMemoryBarrier2 toUpscaler {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                toUpscaler.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                toUpscaler.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                toUpscaler.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                toUpscaler.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                VkDependencyInfo upscalerDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                upscalerDependency.memoryBarrierCount = 1;
                upscalerDependency.pMemoryBarriers = &toUpscaler;
                vkCmdPipelineBarrier2(cmd, &upscalerDependency);

                graphics::FsrUpscaler::Inputs fsrInputs;
                fsrInputs.color = _fsrColor.get();
                fsrInputs.depth = aux[7].get();
                fsrInputs.motion = aux[8].get();
                fsrInputs.output = _fsrOutput.get();
                // The same sub-pixel offset NRD is given: FSR's jitter
                // convention and ours already agree, both being a pixel-space
                // Halton(2,3) with y negated for the UV-down axis.
                const float verticalFov =
                    2.0f * std::atan(1.0f / std::max(1e-4f, projection[1][1]));
                // Read the planes back out of the matrix rather than plumbing
                // them down: unprojecting both ends of the depth range is
                // convention-agnostic, which matters because this projection
                // has already been through glToVulkanClip.
                const glm::mat4 projectionInv = glm::inverse(projection);
                const glm::vec4 nearH = projectionInv * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                const glm::vec4 farH = projectionInv * glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
                const float cameraNear = std::abs(nearH.z / nearH.w);
                const float cameraFar = std::abs(farH.z / farH.w);
                _fsr->dispatch(cmd, fsrInputs, jitterPixels, 1.0f / 60.0f,
                               cameraNear, cameraFar, verticalFov, _options.ptFsrSharpness,
                               _frameNumber == 0 || temporalReset);

                // The backend deliberately leaves its inputs ready for sampled
                // reads. The trace and composite passes write these images as
                // storage images again on the next frame, so restore GENERAL.
                std::array<VkImageMemoryBarrier2, 3> restoreFsrInputs {};
                VulkanImage *fsrInputsToRestore[] {_fsrColor.get(), aux[7].get(), aux[8].get()};
                for (uint32_t i = 0; i < restoreFsrInputs.size(); ++i) {
                    auto &barrier = restoreFsrInputs[i];
                    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    barrier.image = fsrInputsToRestore[i]->handle();
                    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    barrier.subresourceRange.levelCount = 1;
                    barrier.subresourceRange.layerCount = 1;
                }
                VkDependencyInfo restoreFsrDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                restoreFsrDependency.imageMemoryBarrierCount =
                    static_cast<uint32_t>(restoreFsrInputs.size());
                restoreFsrDependency.pImageMemoryBarriers = restoreFsrInputs.data();
                vkCmdPipelineBarrier2(cmd, &restoreFsrDependency);

                VkMemoryBarrier2 toTonemap {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                toTonemap.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                toTonemap.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                toTonemap.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                toTonemap.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
                VkDependencyInfo tonemapDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                tonemapDependency.memoryBarrierCount = 1;
                tonemapDependency.pMemoryBarriers = &toTonemap;
                vkCmdPipelineBarrier2(cmd, &tonemapDependency);

                const auto tonemapSet = _tonemapSets[frameIndex];
                std::array<VkDescriptorImageInfo, 2> tonemapImages {{
                    {VK_NULL_HANDLE, output.view(), VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, _fsrOutput->view(), VK_IMAGE_LAYOUT_GENERAL},
                }};
                std::array<VkWriteDescriptorSet, 2> tonemapWrites {};
                for (uint32_t i = 0; i < 2; ++i) {
                    tonemapWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    tonemapWrites[i].dstSet = tonemapSet;
                    tonemapWrites[i].dstBinding = i;
                    tonemapWrites[i].descriptorCount = 1;
                    tonemapWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    tonemapWrites[i].pImageInfo = &tonemapImages[i];
                }
                vkUpdateDescriptorSets(device.handle(), 2, tonemapWrites.data(), 0, nullptr);
                struct TonemapPush {
                    uint32_t tonemap;
                    float exposure;
                } tonemapPush {static_cast<uint32_t>(std::clamp(_options.ptTonemap, 0, 1)),
                               std::max(0.01f, _options.ptExposure)};
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _tonemapPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _tonemapPipelineLayout,
                                        0, 1, &uniformSet,
                                        static_cast<uint32_t>(offsets.size()), offsets.data());
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _tonemapPipelineLayout,
                                        1, 1, &tonemapSet, 0, nullptr);
                vkCmdPushConstants(cmd, _tonemapPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(tonemapPush), &tonemapPush);
                vkCmdDispatch(cmd, static_cast<uint32_t>((_extent.x + 7) / 8),
                              static_cast<uint32_t>((_extent.y + 7) / 8), 1);
            }
#endif
        }
    }
#endif
    ++_frameNumber;
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
    // The GPU counters only accumulate while the stats flag is on; printing
    // their zeros as rates would read as a lighting regression. Only the
    // secondary ray counters survive the module refactor - the per-light
    // rates belonged to the retired multiplicity estimator.
    std::string statsPart =
        _options.ptTraceStats
            ? "previous frame secondary misses " + std::to_string(_lastSecondaryMisses) +
                  "/" + std::to_string(_lastSecondaryRays) + "; "
            : "trace stats off; ";
    info("Vulkan: TLAS " + std::to_string(_lastInstances) + " instances, " +
             std::to_string(_lastTriangles / 1000) + "k triangles (" +
             std::to_string(_lastDynamicTriangles / 1000) + "k dynamic), " +
             std::to_string(_lastSkinned) + " skinned, skipped " +
             std::to_string(_lastDeforming) + " deforming and " +
             std::to_string(_lastOutOfRange) + " out-of-range meshes, build recorded in " +
             std::to_string(microseconds) + " us; " + std::to_string(_lastEmissive) +
             " emissive, " + std::to_string(_lastAdditive) + " additive, " +
             std::to_string(_lastSabers) + " saber, " + std::to_string(_lastDangly) + " dangly, " +
             std::to_string(_lastGrass) + " grass clusters, " + std::to_string(_lastParticles) + " particles, " +
             std::to_string(_lastBillboards) + " billboards, " + std::to_string(_lastSky) + " sky; " + statsPart +
             std::to_string(_lastBindlessTextureCount) + " bindless 2D textures; " +
             std::to_string(std::max(1, _options.pathTracingSamples)) + " spp",
         LogChannel::Graphics);
}
} // namespace reone::scene
