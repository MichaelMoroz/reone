/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/scene/render/pipeline/rayquery.h"

#include <algorithm>

#include "reone/graphics/options.h"

#include "reone/graphics/vulkan/accelerationstructure.h"
#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/scene/registry.h"
#include "reone/system/logutil.h"

#include <chrono>
#include <cstddef>
#include <cstring>

using namespace reone::graphics;

namespace reone::scene {
namespace {

struct alignas(16) InstanceMaterial {
    glm::vec4 selfIllumColor {0.0f};
    glm::vec4 diffuseColor {1.0f};
    glm::vec4 uv0 {1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec4 uv1 {0.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 uv2 {0.0f};
    uint64_t vertexAddress {0};
    uint64_t indexAddress {0};
    uint32_t vertexStride {0};
    int32_t offPosition {-1};
    int32_t offNormals {-1};
    int32_t offUV1 {-1};
    int32_t offUV2 {-1};
    int32_t offTanSpace {-1};
    uint32_t mainTex {UINT32_MAX};
    uint32_t normalMap {UINT32_MAX};
    uint32_t lightmap {UINT32_MAX};
    uint32_t bumpMapArray {UINT32_MAX};
    uint32_t featureMask {0};
    int32_t bumpMapFrame {0};
    float bumpMapScale {1.0f};
};

static_assert(offsetof(InstanceMaterial, vertexAddress) == 80);
static_assert(offsetof(InstanceMaterial, mainTex) == 120);
static_assert(sizeof(InstanceMaterial) == 160);

struct TraceStats {
    uint32_t secondaryRays {0};
    uint32_t secondaryMisses {0};
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
} // namespace

RayQueryPipeline::RayQueryPipeline(VulkanRenderer &renderer,
                                   glm::ivec2 extent,
                                   GraphicsOptions &options) :
    _renderer(renderer), _options(options), _extent(extent) {}

void RayQueryPipeline::init() {
    if (_inited) return;
    auto &device = _renderer.device();
    _bindlessTextureCapacity = device.maxBindlessSampledImages();
    if (_bindlessTextureCapacity == 0) {
        throw std::runtime_error("Vulkan: ray-query bindless texture capacity is zero");
    }
    VkDescriptorSetLayoutBinding bindings[6] {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = _bindlessTextureCapacity;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = _bindlessTextureCapacity;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorBindingFlags bindingFlags[6] {};
    bindingFlags[4] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    // Vulkan permits only the highest binding to have a variable descriptor
    // count. Binding 4 is still a runtime array in the shader, allocated here
    // at its full capacity; the array-texture binding carries the variable flag.
    bindingFlags[5] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                      VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bindingFlagsInfo.bindingCount = 6;
    bindingFlagsInfo.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.pNext = &bindingFlagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 6;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device.handle(), &layoutInfo, nullptr, &_layout) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: ray-query descriptor layout creation failed");

    VkDescriptorPoolSize sizes[] {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
                                  {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                   4 * _bindlessTextureCapacity}};
    VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 2; poolInfo.poolSizeCount = 4; poolInfo.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device.handle(), &poolInfo, nullptr, &_pool) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: ray-query descriptor pool creation failed");
    VkDescriptorSetAllocateInfo alloc {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    std::array<VkDescriptorSetLayout, 2> setLayouts {_layout, _layout};
    std::array<uint32_t, 2> variableCounts {_bindlessTextureCapacity, _bindlessTextureCapacity};
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableCountInfo {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    variableCountInfo.descriptorSetCount = static_cast<uint32_t>(variableCounts.size());
    variableCountInfo.pDescriptorCounts = variableCounts.data();
    alloc.pNext = &variableCountInfo;
    alloc.descriptorPool = _pool; alloc.descriptorSetCount = static_cast<uint32_t>(setLayouts.size());
    alloc.pSetLayouts = setLayouts.data();
    if (vkAllocateDescriptorSets(device.handle(), &alloc, _sets.data()) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: ray-query descriptor allocation failed");

    auto spirv = readSpirV(_renderer.shaderDir() / "rayquery.spv");
    VkShaderModuleCreateInfo moduleInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = spirv.size() * sizeof(uint32_t); moduleInfo.pCode = spirv.data();
    VkShaderModule module;
    if (vkCreateShaderModule(device.handle(), &moduleInfo, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: ray-query shader module creation failed");
    VkDescriptorSetLayout layouts[] {_renderer.descriptors().uniformLayout(), _layout};
    VkPushConstantRange pushConstants {};
    pushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstants.size = sizeof(TracePushConstants);
    VkPipelineLayoutCreateInfo pipelineLayout {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayout.setLayoutCount = 2; pipelineLayout.pSetLayouts = layouts;
    pipelineLayout.pushConstantRangeCount = 1;
    pipelineLayout.pPushConstantRanges = &pushConstants;
    if (vkCreatePipelineLayout(device.handle(), &pipelineLayout, nullptr, &_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: ray-query pipeline layout creation failed");
    }
    VkPipelineShaderStageCreateInfo stage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; stage.module = module; stage.pName = "main";
    VkComputePipelineCreateInfo pipeline {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage = stage; pipeline.layout = _pipelineLayout;
    if (vkCreateComputePipelines(device.handle(), VK_NULL_HANDLE, 1, &pipeline, nullptr, &_pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: ray-query compute pipeline creation failed");
    }
    vkDestroyShaderModule(device.handle(), module, nullptr);
    device.setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(_pipeline), "rayquery:primaryRay");
    _inited = true;
}

void RayQueryPipeline::clearFrame(Frame &frame) {
    if (frame.tlas) vkDestroyAccelerationStructureKHR(_renderer.device().handle(), frame.tlas, nullptr);
    frame.tlas = VK_NULL_HANDLE; frame.instances.reset(); frame.materials.reset(); frame.traceStats.reset();
    frame.storage.reset(); frame.scratch.reset(); frame.capacity = 0;
}

void RayQueryPipeline::deinit() {
    for (auto &frame : _frames) clearFrame(frame);
    auto &device = _renderer.device();
    if (_pipeline) vkDestroyPipeline(device.handle(), _pipeline, nullptr);
    if (_pipelineLayout) vkDestroyPipelineLayout(device.handle(), _pipelineLayout, nullptr);
    if (_pool) vkDestroyDescriptorPool(device.handle(), _pool, nullptr);
    if (_layout) vkDestroyDescriptorSetLayout(device.handle(), _layout, nullptr);
    _pipeline = VK_NULL_HANDLE; _pipelineLayout = VK_NULL_HANDLE; _pool = VK_NULL_HANDLE; _layout = VK_NULL_HANDLE;
    _bindlessTextureCapacity = 0;
    _lastBindlessTextureCount = 0;
    _inited = false;
}

void RayQueryPipeline::render(VkCommandBuffer cmd, RenderRegistry &registry, uint32_t globalsOffset,
                              VulkanImage &output) {
    // This intentionally bypasses drawScene: its frustum/distance policy must
    // not decide what a ray can hit. Keep the explicit policy construction as
    // the documented caller of the no-culling mode.
    const auto visibility = VisibilityPolicy::noCulling();
    (void)visibility;
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    std::vector<InstanceMaterial> materials;
    instances.reserve(registry.objects().size());
    materials.reserve(registry.objects().size());
    _lastDeforming = 0;
    _lastOutOfRange = 0;
    _lastEmissive = 0;
    for (const auto &object : registry.objects()) {
        const auto *mesh = std::get_if<RegisteredMesh>(&object);
        if (!mesh) continue;
        if (!std::holds_alternative<std::monostate>(mesh->deformation)) { ++_lastDeforming; continue; }
        if (mesh->id.index > 0x00ffffffu) { ++_lastOutOfRange; continue; }
        const auto &blas = _renderer.resources().blas(mesh->mesh.get());
        const auto geometry = _renderer.resources().get(mesh->mesh.get()).geometry();
        VkAccelerationStructureInstanceKHR instance {};
        instance.transform = instanceTransform(mesh->transform);
        instance.instanceCustomIndex = mesh->id.index;
        instance.mask = 0xff;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        VkAccelerationStructureDeviceAddressInfoKHR address {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        address.accelerationStructure = blas.handle();
        instance.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(_renderer.device().handle(), &address);
        instances.push_back(instance);
        InstanceMaterial material;
        material.selfIllumColor = glm::vec4(mesh->material.selfIllumColor, 0.0f);
        material.diffuseColor = glm::vec4(mesh->material.diffuseColor, 1.0f);
        material.uv0 = mesh->material.uv[0];
        material.uv1 = mesh->material.uv[1];
        material.uv2 = mesh->material.uv[2];
        material.vertexAddress = geometry.vertexAddress;
        material.indexAddress = geometry.indexAddress;
        material.vertexStride = static_cast<uint32_t>(geometry.vertexStride);
        const auto &layout = mesh->mesh.get().vertexLayout();
        material.offPosition = static_cast<int32_t>(geometry.positionOffset);
        material.offNormals = layout.offNormals;
        material.offUV1 = layout.offUV1;
        material.offUV2 = layout.offUV2;
        material.offTanSpace = layout.offTanSpace;
        material.featureMask = static_cast<uint32_t>(materialFeatureMask(mesh->material));
        if (const auto *texture = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
            material.mainTex = _renderer.resources().textureId(*texture).value_or(UINT32_MAX);
        } else {
            // No diffuse at all is not an accident: shadow-proxy meshes
            // register with render false and empty texture slots, and they are
            // the only rigid stand-in the TLAS has for a skinned body until
            // the deformation pass exists. They shade from diffuseColor.
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
        materials.push_back(material);
        // Must match isEmitter in slang/rayquery.slang. This was a luma
        // threshold while the shader used one too; when the shader started
        // taking the colour directly, this count silently stopped describing
        // what was actually being traced.
        if (glm::any(glm::greaterThan(mesh->material.selfIllumColor, glm::vec3(0.0f)))) {
            ++_lastEmissive;
        }
    }
    _lastInstances = static_cast<uint32_t>(instances.size());
    if (instances.empty()) {
        // The splash/menu has no scene snapshot. It is not an error, and must
        // not prevent a later module frame from constructing its TLAS.
        VkClearColorValue clear {{0.02f, 0.03f, 0.06f, 1.0f}};
        VkImageSubresourceRange range {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(cmd, output.handle(), VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        return;
    }

    auto &frame = _frames[_renderer.frameIndex()];
    // The renderer waited this in-flight frame's fence before calling us, so
    // its previous GPU-written counters are now safe to inspect.
    if (frame.traceStats) {
        frame.traceStats->invalidateMapped();
        const auto *stats = static_cast<const TraceStats *>(frame.traceStats->mapped());
        _lastSecondaryRays = stats->secondaryRays;
        _lastSecondaryMisses = stats->secondaryMisses;
    }
    clearFrame(frame);
    auto &device = _renderer.device();
    const auto instanceSize = static_cast<VkDeviceSize>(instances.size() * sizeof(instances[0]));
    frame.instances = std::make_unique<VulkanBuffer>(device);
    frame.instances->initHostVisible(instanceSize, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    std::memcpy(frame.instances->mapped(), instances.data(), static_cast<size_t>(instanceSize));
    frame.materials = std::make_unique<VulkanBuffer>(device);
    frame.materials->initHostVisible(static_cast<VkDeviceSize>(materials.size() * sizeof(materials[0])),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(frame.materials->mapped(), materials.data(), materials.size() * sizeof(materials[0]));
    frame.traceStats = std::make_unique<VulkanBuffer>(device);
    frame.traceStats->initHostVisibleReadback(sizeof(TraceStats), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(frame.traceStats->mapped(), 0, sizeof(TraceStats));
    VkAccelerationStructureGeometryInstancesDataKHR instanceData {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instanceData.arrayOfPointers = VK_FALSE; instanceData.data.deviceAddress = frame.instances->deviceAddress();
    VkAccelerationStructureGeometryKHR geometry {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR; geometry.geometry.instances = instanceData;
    VkAccelerationStructureBuildGeometryInfoKHR build {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.geometryCount = 1; build.pGeometries = &geometry;
    const uint32_t count = static_cast<uint32_t>(instances.size());
    VkAccelerationStructureBuildSizesInfoKHR sizes {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                             &build, &count, &sizes);
    frame.storage = std::make_unique<VulkanBuffer>(device);
    frame.storage->initDeviceLocal(sizes.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr);
    VkAccelerationStructureCreateInfoKHR create {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.buffer = frame.storage->handle(); create.size = sizes.accelerationStructureSize;
    create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(device.handle(), &create, nullptr, &frame.tlas) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: TLAS creation failed");
    const auto alignment = device.accelerationStructureProperties().minAccelerationStructureScratchOffsetAlignment;
    frame.scratch = std::make_unique<VulkanBuffer>(device);
    frame.scratch->initDeviceLocal(sizes.buildScratchSize + alignment,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr);
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.dstAccelerationStructure = frame.tlas;
    build.scratchData.deviceAddress = alignedAddress(frame.scratch->deviceAddress(), alignment);
    VkAccelerationStructureBuildRangeInfoKHR range {}; range.primitiveCount = count;
    const VkAccelerationStructureBuildRangeInfoKHR *ranges[] {&range};
    const auto begin = std::chrono::steady_clock::now();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, ranges);
    VkMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkDescriptorImageInfo image {}; image.imageView = output.view(); image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSetAccelerationStructureKHR asWrite {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asWrite.accelerationStructureCount = 1; asWrite.pAccelerationStructures = &frame.tlas;
    VkDescriptorBufferInfo materialBuffer {};
    materialBuffer.buffer = frame.materials->handle();
    materialBuffer.range = frame.materials->size();
    VkDescriptorBufferInfo statsBuffer {};
    statsBuffer.buffer = frame.traceStats->handle();
    statsBuffer.range = frame.traceStats->size();
    VkWriteDescriptorSet writes[4] {};
    const auto set = _sets[_renderer.frameIndex()];
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = set; writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[0].pImageInfo = &image;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].pNext = &asWrite; writes[1].dstSet = set; writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[2].dstSet = set; writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &materialBuffer;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[3].dstSet = set; writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[3].pBufferInfo = &statsBuffer;
    vkUpdateDescriptorSets(device.handle(), 4, writes, 0, nullptr);
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
        textureWrite.dstBinding = 4;
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
        textureWrite.dstBinding = 5;
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
    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[0] = globalsOffset;
    auto uniformSet = _renderer.uniformSet();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 1, 1, &set, 0, nullptr);
    // Clamped rather than trusted: the option is user-editable in reone.cfg
    // and a zero would divide the accumulated radiance by zero.
    TracePushConstants constants {_frameNumber,
                                  static_cast<uint32_t>(std::max(1, _options.pathTracingSamples))};
    vkCmdPushConstants(cmd, _pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(cmd, static_cast<uint32_t>((_extent.x + 7) / 8), static_cast<uint32_t>((_extent.y + 7) / 8), 1);
    ++_frameNumber;
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
    info("Vulkan: TLAS " + std::to_string(_lastInstances) + " instances, skipped " +
         std::to_string(_lastDeforming) + " deforming and " +
         std::to_string(_lastOutOfRange) + " out-of-range meshes, build recorded in " +
         std::to_string(microseconds) + " us; " + std::to_string(_lastEmissive) +
         " emissive; previous frame secondary misses " + std::to_string(_lastSecondaryMisses) +
         "/" + std::to_string(_lastSecondaryRays) + "; " +
         std::to_string(_lastBindlessTextureCount) + " bindless 2D textures; " +
         std::to_string(std::max(1, _options.pathTracingSamples)) + " spp", LogChannel::Graphics);
}
} // namespace reone::scene
