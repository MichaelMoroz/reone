/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/scene/render/pipeline/rayquery.h"

#include <algorithm>

#include "reone/graphics/options.h"
#include "reone/graphics/uniforms.h"

#include "reone/graphics/vulkan/accelerationstructure.h"
#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/scene/node/model.h"
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
    VkDescriptorSetLayoutBinding bindings[7] {};
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
    // Per-category material overrides.
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = _bindlessTextureCapacity;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorCount = _bindlessTextureCapacity;
    bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorBindingFlags bindingFlags[7] {};
    bindingFlags[5] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    // Vulkan permits only the highest binding to have a variable descriptor
    // count. Binding 5 is still a runtime array in the shader, allocated here
    // at its full capacity; the array-texture binding carries the variable flag.
    bindingFlags[6] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                      VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bindingFlagsInfo.bindingCount = 7;
    bindingFlagsInfo.pBindingFlags = bindingFlags;
    VkDescriptorSetLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.pNext = &bindingFlagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 7;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device.handle(), &layoutInfo, nullptr, &_layout) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: ray-query descriptor layout creation failed");

    VkDescriptorPoolSize sizes[] {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
                                  {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6},
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

    VkDescriptorSetLayoutBinding skinBindings[2] {};
    for (uint32_t i = 0; i < 2; ++i) {
        skinBindings[i].binding = i;
        skinBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        skinBindings[i].descriptorCount = 1;
        skinBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo skinLayoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    skinLayoutInfo.bindingCount = 2;
    skinLayoutInfo.pBindings = skinBindings;
    if (vkCreateDescriptorSetLayout(device.handle(), &skinLayoutInfo, nullptr, &_skinLayout) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: skin descriptor layout creation failed");
    constexpr uint32_t kMaxSkinnedInstancesPerFrame = 128;
    for (auto &pool : _skinPools) {
        VkDescriptorPoolSize skinPoolSize {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           2 * kMaxSkinnedInstancesPerFrame};
        VkDescriptorPoolCreateInfo skinPoolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        skinPoolInfo.maxSets = kMaxSkinnedInstancesPerFrame;
        skinPoolInfo.poolSizeCount = 1;
        skinPoolInfo.pPoolSizes = &skinPoolSize;
        if (vkCreateDescriptorPool(device.handle(), &skinPoolInfo, nullptr, &pool) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: skin descriptor pool creation failed");
    }
    auto skinSpirv = readSpirV(_renderer.shaderDir() / "skin.spv");
    moduleInfo.codeSize = skinSpirv.size() * sizeof(uint32_t);
    moduleInfo.pCode = skinSpirv.data();
    if (vkCreateShaderModule(device.handle(), &moduleInfo, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: skin shader module creation failed");
    VkDescriptorSetLayout skinLayouts[] {_renderer.descriptors().uniformLayout(), _skinLayout};
    VkPushConstantRange skinPushConstants {};
    skinPushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    skinPushConstants.size = sizeof(SkinPushConstants);
    VkPipelineLayoutCreateInfo skinPipelineLayoutInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    skinPipelineLayoutInfo.setLayoutCount = 2;
    skinPipelineLayoutInfo.pSetLayouts = skinLayouts;
    skinPipelineLayoutInfo.pushConstantRangeCount = 1;
    skinPipelineLayoutInfo.pPushConstantRanges = &skinPushConstants;
    if (vkCreatePipelineLayout(device.handle(), &skinPipelineLayoutInfo, nullptr, &_skinPipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: skin pipeline layout creation failed");
    }
    stage.module = module;
    // VkComputePipelineCreateInfo owns a value copy of the stage descriptor;
    // replacing the module for the skin pipeline must replace that copy too.
    pipeline.stage = stage;
    pipeline.layout = _skinPipelineLayout;
    if (vkCreateComputePipelines(device.handle(), VK_NULL_HANDLE, 1, &pipeline, nullptr, &_skinPipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: skin compute pipeline creation failed");
    }
    vkDestroyShaderModule(device.handle(), module, nullptr);
    device.setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(_skinPipeline), "rayquery:skin");
    _inited = true;
}

void RayQueryPipeline::clearFrame(Frame &frame) {
    for (auto &skinned : frame.skinned) {
        if (skinned.blas) vkDestroyAccelerationStructureKHR(_renderer.device().handle(), skinned.blas, nullptr);
    }
    frame.skinned.clear();
    if (frame.tlas) vkDestroyAccelerationStructureKHR(_renderer.device().handle(), frame.tlas, nullptr);
    frame.tlas = VK_NULL_HANDLE; frame.instances.reset(); frame.materials.reset(); frame.traceStats.reset();
    frame.storage.reset(); frame.scratch.reset(); frame.capacity = 0;
}

void RayQueryPipeline::deinit() {
    for (auto &frame : _frames) clearFrame(frame);
    auto &device = _renderer.device();
    if (_skinPipeline) vkDestroyPipeline(device.handle(), _skinPipeline, nullptr);
    if (_skinPipelineLayout) vkDestroyPipelineLayout(device.handle(), _skinPipelineLayout, nullptr);
    for (auto &pool : _skinPools) {
        if (pool) vkDestroyDescriptorPool(device.handle(), pool, nullptr);
    }
    if (_skinLayout) vkDestroyDescriptorSetLayout(device.handle(), _skinLayout, nullptr);
    if (_pipeline) vkDestroyPipeline(device.handle(), _pipeline, nullptr);
    if (_pipelineLayout) vkDestroyPipelineLayout(device.handle(), _pipelineLayout, nullptr);
    if (_pool) vkDestroyDescriptorPool(device.handle(), _pool, nullptr);
    if (_layout) vkDestroyDescriptorSetLayout(device.handle(), _layout, nullptr);
    _pipeline = VK_NULL_HANDLE; _pipelineLayout = VK_NULL_HANDLE; _pool = VK_NULL_HANDLE; _layout = VK_NULL_HANDLE;
    _skinPipeline = VK_NULL_HANDLE; _skinPipelineLayout = VK_NULL_HANDLE; _skinLayout = VK_NULL_HANDLE;
    _skinPools = {};
    _bindlessTextureCapacity = 0;
    _lastBindlessTextureCount = 0;
    _inited = false;
}

VulkanMesh::Geometry RayQueryPipeline::skin(VkCommandBuffer cmd,
                                             Frame &frame,
                                             const VulkanMesh &source,
                                             const Mesh::VertexLayout &layout,
                                             const RegisteredSkin &skin,
                                             uint32_t globalsOffset) {
    const auto sourceGeometry = source.geometry();
    if (sourceGeometry.vertexStride % sizeof(float) != 0) {
        throw std::runtime_error("Vulkan: skinned vertex stride is not float-aligned");
    }
    auto &result = frame.skinned.emplace_back();
    result.vertices = std::make_unique<VulkanBuffer>(_renderer.device());
    result.vertices->initDeviceLocal(source.vertexDataSize(),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        nullptr);

    VkDescriptorSetAllocateInfo allocateInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = _skinPools[_renderer.frameIndex()];
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &_skinLayout;
    VkDescriptorSet set {VK_NULL_HANDLE};
    if (vkAllocateDescriptorSets(_renderer.device().handle(), &allocateInfo, &set) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: skin descriptor allocation failed");
    VkDescriptorBufferInfo sourceInfo {source.vertexBuffer(), 0, source.vertexDataSize()};
    VkDescriptorBufferInfo destinationInfo {result.vertices->handle(), 0, result.vertices->size()};
    VkWriteDescriptorSet writes[2] {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    writes[0].pBufferInfo = &sourceInfo;
    writes[1].pBufferInfo = &destinationInfo;
    vkUpdateDescriptorSets(_renderer.device().handle(), 2, writes, 0, nullptr);

    BoneUniforms bones;
    for (size_t i = 0; i < skin.bones.size() && i < kMaxBones; ++i) {
        bones.bones[i] = skin.bones[i];
    }
    const auto bonesOffset = _renderer.uniformRing().push(bones);
    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
    offsets[0] = globalsOffset;
    offsets[UniformBlockBindingPoints::bones] = bonesOffset;
    const auto uniformSet = _renderer.uniformSet();
    SkinPushConstants constants {
        sourceGeometry.maxVertexIndex + 1,
        static_cast<uint32_t>(sourceGeometry.vertexStride / sizeof(float)),
        layout.offPosition / static_cast<int>(sizeof(float)),
        layout.offNormals / static_cast<int>(sizeof(float)),
        layout.offBoneIndices / static_cast<int>(sizeof(float)),
        layout.offBoneWeights / static_cast<int>(sizeof(float)),
        // -1 must stay -1: integer division would fold "absent" onto the
        // position offset and skin garbage over it.
        layout.offTanSpace >= 0 ? layout.offTanSpace / static_cast<int>(sizeof(float)) : -1};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _skinPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _skinPipelineLayout, 0, 1, &uniformSet,
                            static_cast<uint32_t>(offsets.size()), offsets.data());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _skinPipelineLayout, 1, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, _skinPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(cmd, (constants.vertexCount + 63) / 64, 1, 1);

    VkMemoryBarrier2 skinBarrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    skinBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    skinBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    // Two consumers, not one: the BLAS build reads positions, and the trace
    // dispatch itself reads UVs, normals, and tangent frames from this buffer
    // through its device address. Guarding only the build left the trace
    // racing the skinning writes - striped garbage UVs across every skinned
    // mesh, different every run.
    skinBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    skinBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo skinDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    skinDependency.memoryBarrierCount = 1;
    skinDependency.pMemoryBarriers = &skinBarrier;
    vkCmdPipelineBarrier2(cmd, &skinDependency);

    VulkanMesh::Geometry geometry = sourceGeometry;
    geometry.vertexAddress = result.vertices->deviceAddress();
    VkAccelerationStructureGeometryTrianglesDataKHR triangles {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    triangles.vertexFormat = geometry.vertexFormat;
    triangles.vertexData.deviceAddress = geometry.vertexAddress + geometry.positionOffset;
    triangles.vertexStride = geometry.vertexStride;
    triangles.maxVertex = geometry.maxVertexIndex;
    triangles.indexType = geometry.indexType;
    triangles.indexData.deviceAddress = geometry.indexAddress;
    VkAccelerationStructureGeometryKHR asGeometry {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    asGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    asGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    asGeometry.geometry.triangles = triangles;
    const uint32_t primitiveCount = source.indexCount() / 3;
    VkAccelerationStructureBuildGeometryInfoKHR build {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    build.geometryCount = 1;
    build.pGeometries = &asGeometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(_renderer.device().handle(),
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitiveCount, &sizes);
    result.storage = std::make_unique<VulkanBuffer>(_renderer.device());
    result.storage->initDeviceLocal(sizes.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr);
    VkAccelerationStructureCreateInfoKHR create {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.buffer = result.storage->handle();
    create.size = sizes.accelerationStructureSize;
    create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(_renderer.device().handle(), &create, nullptr, &result.blas) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: skinned BLAS creation failed");
    const auto alignment = _renderer.device().accelerationStructureProperties().minAccelerationStructureScratchOffsetAlignment;
    result.scratch = std::make_unique<VulkanBuffer>(_renderer.device());
    result.scratch->initDeviceLocal(sizes.buildScratchSize + alignment,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr);
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.dstAccelerationStructure = result.blas;
    build.scratchData.deviceAddress = alignedAddress(result.scratch->deviceAddress(), alignment);
    VkAccelerationStructureBuildRangeInfoKHR range {};
    range.primitiveCount = primitiveCount;
    const VkAccelerationStructureBuildRangeInfoKHR *ranges[] {&range};
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, ranges);
    return geometry;
}

void RayQueryPipeline::render(VkCommandBuffer cmd, RenderRegistry &registry, uint32_t globalsOffset,
                              VulkanImage &output) {
    // This intentionally bypasses drawScene: its frustum/distance policy must
    // not decide what a ray can hit. Keep the explicit policy construction as
    // the documented caller of the no-culling mode.
    const auto visibility = VisibilityPolicy::noCulling();
    (void)visibility;
    auto &frame = _frames[_renderer.frameIndex()];
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
    vkResetDescriptorPool(_renderer.device().handle(), _skinPools[_renderer.frameIndex()], 0);
    std::vector<VkAccelerationStructureInstanceKHR> instances;
    std::vector<InstanceMaterial> materials;
    instances.reserve(registry.objects().size());
    materials.reserve(registry.objects().size());
    _lastDeforming = 0;
    _lastSkinned = 0;
    _lastOutOfRange = 0;
    _lastEmissive = 0;
    _lastAdditive = 0;
    _lastSabers = 0;
    _lastDangly = 0;
    for (const auto &object : registry.objects()) {
        const auto *mesh = std::get_if<RegisteredMesh>(&object);
        if (!mesh) continue;
        // Shadow-only entries - render flag off, categories reduced to
        // ShadowCaster - are the simplified shadow-volume proxies Odyssey
        // ships inside character models: skin-tight untextured boxes around
        // the skeleton. Raster only ever draws them into shadow maps; traced
        // as geometry they render as white patches over the real body, and
        // traced shadows already test the real surfaces.
        if ((mesh->categories & (renderCategory(RenderCategory::Opaque) |
                                 renderCategory(RenderCategory::Transparent))) == 0) {
            continue;
        }
        // Saber displacement is a small whole-blade animation. A rigid blade
        // is much more useful to tracing than no blade at all. Skinned meshes
        // take the frame-local compute/BLAS path below.
        const bool saber = std::holds_alternative<RegisteredSaber>(mesh->deformation);
        // Dangly meshes are admitted at their base positions for the same
        // reason sabers are: a static canopy beats an absent one, and the
        // per-frame displacement is small. This is why every tree has leaves
        // rather than only those whose canopy happens to be rigid. The wind
        // arrives with the deformation compute pass, which replaces this.
        const bool dangly = std::holds_alternative<RegisteredDangly>(mesh->deformation);
        const auto *skinned = std::get_if<RegisteredSkin>(&mesh->deformation);
        if (!std::holds_alternative<std::monostate>(mesh->deformation) && !skinned && !saber && !dangly) {
            ++_lastDeforming;
            continue;
        }
        if (saber) ++_lastSabers;
        if (dangly) ++_lastDangly;
        if (mesh->id.index > 0x00ffffffu) { ++_lastOutOfRange; continue; }
        const auto &uploaded = _renderer.resources().get(mesh->mesh.get());
        VulkanMesh::Geometry geometry;
        VkAccelerationStructureKHR blasHandle {VK_NULL_HANDLE};
        if (skinned) {
            geometry = skin(cmd, frame, uploaded, mesh->mesh.get().vertexLayout(), *skinned, globalsOffset);
            blasHandle = frame.skinned.back().blas;
            ++_lastSkinned;
        } else {
            const auto &blas = _renderer.resources().blas(mesh->mesh.get());
            geometry = uploaded.geometry();
            blasHandle = blas.handle();
        }
        VkAccelerationStructureInstanceKHR instance {};
        instance.transform = instanceTransform(mesh->transform);
        instance.instanceCustomIndex = mesh->id.index;
        // Bit 1 is the world, bit 2 the sky. Camera and bounce rays trace
        // with both; shadow rays cull bit 2, because the sky dome is an
        // environment, not an occluder - with a plain 0xff mask every sun
        // shadow ray committed on the dome and the promoted sun lit nothing.
        // Reassigned below once the material is classified.
        instance.mask = 0x1;
        instance.instanceShaderBindingTableRecordOffset = 0;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        VkAccelerationStructureDeviceAddressInfoKHR address {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        address.accelerationStructure = blasHandle;
        instance.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(_renderer.device().handle(), &address);
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
        // Bits 27-30 carry the object category - a scene::ModelUsage value,
        // 8 for meshes without a model root - so per-category material
        // overrides resolve at hit time without touching the layout. Must
        // match kTraceCategoryShift/Mask in slang/rayquery.slang.
        uint32_t categoryIndex = mesh->cullRoot
                                     ? static_cast<uint32_t>(mesh->cullRoot->usage())
                                     : 8u;
        material.featureMask |= (categoryIndex & 0xFu) << 27;
        // Sky is fully self-illuminated geometry - the same luma test
        // isTransparent uses. A tracing-local bit, deliberately above the
        // shared UniformsFeatureFlags range; must match kTraceSky in
        // slang/rayquery.slang.
        if (glm::dot(mesh->material.selfIllumColor, glm::vec3(0.299f, 0.587f, 0.114f)) >= 0.99f) {
            material.featureMask |= 1u << 24;
            instance.mask = 0x2;
        }
        // Additive-blended diffuse is the other way Odyssey authors a glow:
        // no selfIllum controller, the texture itself is the light, and the
        // raster path treats it as unlit for the same reason. Without this
        // bit every indicator lamp and glow decal traces as a dark surface.
        // Must match kTraceAdditive in slang/rayquery.slang.
        if (const auto *diffuse = mesh->material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
            if (diffuse->features().blending == Texture::Blending::Additive) {
                material.featureMask |= 1u << 25;
                ++_lastAdditive;
            } else if (diffuse->features().blending == Texture::Blending::PunchThrough ||
                       mesh->material.type == MaterialType::TransparentModel) {
                // Not only authored punch-through: any transparent,
                // non-additive mesh - alpha-blended leaves above all - carries
                // its coverage in the diffuse alpha and must composite as
                // layers. Keying only on PunchThrough left Normal-blended
                // canopies fully opaque in the traced view.
                // Candidate alpha is composited deterministically in the
                // shader, so hardware must not accept the triangle first.
                material.featureMask |= 1u << 26;
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
        if ((material.featureMask & ((1u << 25) | (1u << 26))) != 0) {
            // Additive surfaces always transmit; punch-through surfaces decide
            // per texel in Proceed(). Both must therefore reach candidates.
            instance.flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
        }
        instances.push_back(instance);
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
    if (!frame.skinned.empty()) {
        // Each dynamic BLAS was written after its compute-to-build barrier.
        // The TLAS consumes those BLAS addresses next, so make the bottom-level
        // writes visible to this top-level build before recording it.
        VkMemoryBarrier2 blasBarrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        blasBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        blasBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        blasBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        blasBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        VkDependencyInfo blasDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        blasDependency.memoryBarrierCount = 1;
        blasDependency.pMemoryBarriers = &blasBarrier;
        vkCmdPipelineBarrier2(cmd, &blasDependency);
    }
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
    // Per-category overrides, refreshed every frame so the ImGui dials are
    // live. Layout must match CategoryOverride in slang/rayquery.slang.
    struct CategoryOverrideGpu {
        float color[4];
        float params[4];
    };
    std::array<CategoryOverrideGpu, 9> overrideData {};
    for (size_t i = 0; i < overrideData.size(); ++i) {
        const auto &src = _options.ptCategoryOverrides[i];
        overrideData[i] = {{src.color[0], src.color[1], src.color[2],
                            std::clamp(src.colorWeight, 0.0f, 1.0f)},
                           {src.roughness,
                            std::max(0.0f, src.emissionScale),
                            std::max(0.0f, src.envScale), 0.0f}};
    }
    frame.overrides = std::make_unique<VulkanBuffer>(device);
    frame.overrides->initHostVisible(sizeof(overrideData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(frame.overrides->mapped(), overrideData.data(), sizeof(overrideData));
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
    VkDescriptorBufferInfo overridesBuffer {};
    overridesBuffer.buffer = frame.overrides->handle();
    overridesBuffer.range = frame.overrides->size();
    VkWriteDescriptorSet writes[5] {};
    const auto set = _sets[_renderer.frameIndex()];
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[0].dstSet = set; writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[0].pImageInfo = &image;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[1].pNext = &asWrite; writes[1].dstSet = set; writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[2].dstSet = set; writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &materialBuffer;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[3].dstSet = set; writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[3].pBufferInfo = &statsBuffer;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[4].dstSet = set; writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1; writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[4].pBufferInfo = &overridesBuffer;
    vkUpdateDescriptorSets(device.handle(), 5, writes, 0, nullptr);
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
        textureWrite.dstBinding = 6;
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
                                  static_cast<uint32_t>(std::max(1, _options.pathTracingSamples)),
                                  std::max(0.0f, _options.ptSkyIntensity),
                                  std::max(0.0f, _options.ptEmissiveIntensity),
                                  std::max(0.0f, _options.ptLightmapIntensity),
                                  std::max(0.0f, _options.ptDirectIntensity),
                                  std::max(0.0001f, _options.ptRayOffset),
                                  std::max(0.0f, _options.ptWorldAmbient),
                                  std::max(0.0f, _options.ptSunIntensity),
                                  (_options.ptTraceStats ? 1u : 0u) |
                                      (static_cast<uint32_t>(std::clamp(_options.ptDebugView, 0, 6)) << 4),
                                  static_cast<uint32_t>(std::clamp(_options.ptBounces, 1, 8))};
    vkCmdPushConstants(cmd, _pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(cmd, static_cast<uint32_t>((_extent.x + 7) / 8), static_cast<uint32_t>((_extent.y + 7) / 8), 1);
    ++_frameNumber;
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
    // The GPU counters only accumulate while the stats flag is on; printing
    // their zeros as rates would read as a lighting regression.
    std::string statsPart =
        _options.ptTraceStats
            ? "previous frame secondary misses " + std::to_string(_lastSecondaryMisses) +
                  "/" + std::to_string(_lastSecondaryRays) + "; " +
                  std::to_string(_lastPrimaryHits ? static_cast<float>(_lastSurvivingLights) / _lastPrimaryHits : 0.0f) +
                  " lights past cutoff/primary hit; " +
                  std::to_string(_lastPrimaryHits ? static_cast<float>(_lastShadowRays) / _lastPrimaryHits : 0.0f) +
                  " direct shadow rays/primary hit; "
            : "trace stats off; ";
    info("Vulkan: TLAS " + std::to_string(_lastInstances) + " instances, " +
         std::to_string(_lastSkinned) + " skinned, skipped " +
         std::to_string(_lastDeforming) + " deforming and " +
         std::to_string(_lastOutOfRange) + " out-of-range meshes, build recorded in " +
         std::to_string(microseconds) + " us; " + std::to_string(_lastEmissive) +
          " emissive, " + std::to_string(_lastAdditive) + " additive, " +
         std::to_string(_lastSabers) + " saber, " + std::to_string(_lastDangly) + " dangly; " + statsPart +
         std::to_string(_lastBindlessTextureCount) + " bindless 2D textures; " +
         std::to_string(std::max(1, _options.pathTracingSamples)) + " spp", LogChannel::Graphics);
}
} // namespace reone::scene
