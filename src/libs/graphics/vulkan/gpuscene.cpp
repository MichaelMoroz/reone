/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/graphics/vulkan/gpuscene.h"

#include "reone/system/profiler.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "reone/graphics/mesh.h"
#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/system/logutil.h"

namespace reone::graphics {
namespace {

struct MergePushConstants {
    uint32_t objectCount;
    uint32_t opaqueObjectCount;
    uint32_t vertexCount;
    uint32_t triangleCount;
    uint32_t opaqueTriangleCount;
    uint32_t pad[3] {};
    glm::vec4 cameraPosition {0.0f};
};
static_assert(sizeof(MergePushConstants) == 48);

uint32_t grownCapacity(uint32_t current, uint32_t required, uint32_t minimum) {
    if (current != 0 && required <= current)
        return current;
    uint64_t capacity = std::max(current, minimum);
    while (capacity < required)
        capacity *= 2;
    if (capacity > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Vulkan: merged scene capacity exceeds uint32 range");
    return static_cast<uint32_t>(capacity);
}

} // namespace

struct VulkanGpuScene::Frame {
    std::unique_ptr<VulkanBuffer> scene;
    std::unique_ptr<VulkanBuffer> geometry;
    std::unique_ptr<VulkanBuffer> proceduralQuads;
    std::unique_ptr<VulkanBuffer> grassRanges;
    std::unique_ptr<VulkanBuffer> materials;
    uint32_t sceneObjectCapacity {0};
    uint32_t boneCapacity {0};
    uint32_t danglyPositionCapacity {0};
    uint32_t vertexCapacity {0};
    uint32_t triangleCapacity {0};
    uint32_t proceduralQuadCapacity {0};
    uint32_t grassRangeCapacity {0};
    std::vector<PrimitiveIdRange> primitiveIds;
};

VulkanGpuScene::VulkanGpuScene() = default;

VulkanGpuScene::~VulkanGpuScene() {
    deinit();
}

VulkanGpuScene::PrimitiveId VulkanGpuScene::PrimitiveIdView::operator[](uint32_t index) const {
    for (uint32_t rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex) {
        const auto &range = ranges[rangeIndex];
        if (index < range.firstTriangle || index - range.firstTriangle >= range.triangleCount)
            continue;
        auto id = range.first;
        id.localPrimitive += index - range.firstTriangle;
        return id;
    }
    throw std::out_of_range("Vulkan: frame-local primitive address is not published");
}

void VulkanGpuScene::init(VulkanRenderer &renderer) {
    if (_inited)
        return;
    _renderer = &renderer;
    auto &device = _renderer->device();
    VkDescriptorSetLayoutBinding bindings[11] {};
    for (uint32_t i = 0; i < 11; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 11;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device.handle(), &layoutInfo, nullptr, &_mergeLayout) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: merge descriptor layout creation failed");
    VkDescriptorPoolSize poolSize {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 22};
    VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device.handle(), &poolInfo, nullptr, &_mergePool) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: merge descriptor pool creation failed");
    std::array<VkDescriptorSetLayout, 2> setLayouts {_mergeLayout, _mergeLayout};
    VkDescriptorSetAllocateInfo alloc {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = _mergePool;
    alloc.descriptorSetCount = static_cast<uint32_t>(setLayouts.size());
    alloc.pSetLayouts = setLayouts.data();
    if (vkAllocateDescriptorSets(device.handle(), &alloc, _mergeSets.data()) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: merge descriptor allocation failed");
    const auto &spirv = _renderer->shaderModule("skin");
    VkShaderModuleCreateInfo moduleInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = spirv.size() * sizeof(uint32_t);
    moduleInfo.pCode = spirv.data();
    VkShaderModule module;
    if (vkCreateShaderModule(device.handle(), &moduleInfo, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: merge shader module creation failed");
    VkPushConstantRange pushConstants {};
    pushConstants.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstants.size = sizeof(MergePushConstants);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &_mergeLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstants;
    if (vkCreatePipelineLayout(device.handle(), &pipelineLayoutInfo, nullptr,
                               &_mergePipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: merge pipeline layout creation failed");
    }
    VkComputePipelineCreateInfo pipelineInfo {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = _mergePipelineLayout;
    const auto result = vkCreateComputePipelines(device.handle(), VK_NULL_HANDLE, 1,
                                                 &pipelineInfo, nullptr, &_mergePipeline);
    vkDestroyShaderModule(device.handle(), module, nullptr);
    if (result != VK_SUCCESS)
        throw std::runtime_error("Vulkan: merge compute pipeline creation failed");
    device.setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(_mergePipeline),
                         "gpu-scene:merge");
    for (auto &frame : _frames)
        frame = std::make_unique<Frame>();
    _inited = true;
}

void VulkanGpuScene::deinit() {
    if (!_inited)
        return;
    auto &device = _renderer->device();
    _frames = {};
    clearSourceGeometry();
    _grassFaces.reset();
    _grassFaceGeneration = 0;
    if (_mergePipeline)
        vkDestroyPipeline(device.handle(), _mergePipeline, nullptr);
    if (_mergePipelineLayout)
        vkDestroyPipelineLayout(device.handle(), _mergePipelineLayout, nullptr);
    if (_mergePool)
        vkDestroyDescriptorPool(device.handle(), _mergePool, nullptr);
    if (_mergeLayout)
        vkDestroyDescriptorSetLayout(device.handle(), _mergeLayout, nullptr);
    _mergePipeline = VK_NULL_HANDLE;
    _mergePipelineLayout = VK_NULL_HANDLE;
    _mergePool = VK_NULL_HANDLE;
    _mergeLayout = VK_NULL_HANDLE;
    _mergeSets = {};
    _renderer = nullptr;
    _inited = false;
}

void VulkanGpuScene::clearSourceGeometry() {
    _sourceGeometry.clear();
    _sourceVertexData.clear();
    _sourceIndexData.clear();
    _sourceVertices.reset();
    _sourceIndices.reset();
    _retiredSourceBuffers.clear();
    _sourceVertexCapacity = 0;
    _sourceIndexCapacity = 0;
}

void VulkanGpuScene::ensureMergeBuffers(Frame &frame, uint32_t objectCount,
                                        uint32_t boneCount, uint32_t vertexCount,
                                        uint32_t triangleCount, uint32_t proceduralQuadCount,
                                        uint32_t danglyPositionCount,
                                        uint32_t grassRangeCount) {
    constexpr uint32_t kInitialObjectCapacity = 64, kInitialBoneCapacity = 256;
    constexpr uint32_t kInitialVertexCapacity = 4096, kInitialTriangleCapacity = 4096;
    const auto objectCapacity =
        grownCapacity(frame.sceneObjectCapacity, objectCount, kInitialObjectCapacity);
    const auto boneCapacity = grownCapacity(frame.boneCapacity, boneCount, kInitialBoneCapacity);
    const auto danglyPositionCapacity =
        grownCapacity(frame.danglyPositionCapacity, danglyPositionCount, 256);
    if (!frame.scene || objectCapacity != frame.sceneObjectCapacity ||
        boneCapacity != frame.boneCapacity ||
        danglyPositionCapacity != frame.danglyPositionCapacity) {
        frame.sceneObjectCapacity = objectCapacity;
        frame.boneCapacity = boneCapacity;
        frame.danglyPositionCapacity = danglyPositionCapacity;
        frame.scene = std::make_unique<VulkanBuffer>(_renderer->device());
        frame.scene->initHostVisible(
            static_cast<VkDeviceSize>(objectCapacity) * sizeof(SceneObject) +
                static_cast<VkDeviceSize>(boneCapacity) * sizeof(GpuSceneMatrix3x4) +
                static_cast<VkDeviceSize>(danglyPositionCapacity) * sizeof(glm::vec4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    const auto vertexCapacity =
        grownCapacity(frame.vertexCapacity, vertexCount, kInitialVertexCapacity);
    const auto triangleCapacity =
        grownCapacity(frame.triangleCapacity, triangleCount, kInitialTriangleCapacity);
    if (!frame.geometry || vertexCapacity != frame.vertexCapacity ||
        triangleCapacity != frame.triangleCapacity) {
        frame.vertexCapacity = vertexCapacity;
        frame.triangleCapacity = triangleCapacity;
        const VkDeviceSize vertexBytes =
            static_cast<VkDeviceSize>(vertexCapacity) * sizeof(MergedVertex);
        const VkDeviceSize indexBytes =
            static_cast<VkDeviceSize>(triangleCapacity) * 3 * sizeof(uint32_t);
        const VkDeviceSize materialIdBytes =
            static_cast<VkDeviceSize>(triangleCapacity) * sizeof(uint32_t);
        frame.geometry = std::make_unique<VulkanBuffer>(_renderer->device());
        // INDEX_BUFFER is for the raster consumer added in F2. Vertices need no
        // new usage because raster pulls them programmably - a
        // StructuredBuffer<MergedVertex> indexed by SV_VertexID - rather than
        // going through vertex input, so there is one declaration of the vertex
        // layout instead of two and no format plumbing in the pipeline key.
        // Indices still bind as indices, which keeps the post-transform cache.
        frame.geometry->initDeviceLocal(
            vertexBytes + indexBytes + materialIdBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            nullptr);
    }
    const auto proceduralQuadCapacity =
        grownCapacity(frame.proceduralQuadCapacity, proceduralQuadCount, 64);
    if (!frame.proceduralQuads || proceduralQuadCapacity != frame.proceduralQuadCapacity) {
        frame.proceduralQuadCapacity = proceduralQuadCapacity;
        frame.proceduralQuads = std::make_unique<VulkanBuffer>(_renderer->device());
        frame.proceduralQuads->initHostVisible(
            static_cast<VkDeviceSize>(proceduralQuadCapacity) * sizeof(GpuSceneProceduralQuad),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    const auto grassRangeCapacity =
        grownCapacity(frame.grassRangeCapacity, grassRangeCount, 64);
    if (!frame.grassRanges || grassRangeCapacity != frame.grassRangeCapacity) {
        frame.grassRangeCapacity = grassRangeCapacity;
        frame.grassRanges = std::make_unique<VulkanBuffer>(_renderer->device());
        frame.grassRanges->initHostVisible(
            static_cast<VkDeviceSize>(grassRangeCapacity) * sizeof(GpuSceneGrassRange),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
}

const VulkanGpuScene::SourceGeometry &VulkanGpuScene::appendSourceGeometry(const Mesh &mesh) {
    const auto [it, inserted] = _sourceGeometry.emplace(&mesh, SourceGeometry {});
    const auto &vertices = mesh.vertexData();
    const auto &faces = mesh.faces();
    if (!inserted) {
        if (it->second.vertexDataCount != vertices.size() ||
            it->second.indexCount != faces.size() * 3) {
            warn("Vulkan: GpuScene source cache reused a Mesh address with different geometry",
                 LogChannel::Graphics);
        }
        return it->second;
    }
    if (vertices.size() > std::numeric_limits<uint32_t>::max() - _sourceVertexData.size() ||
        faces.size() > (std::numeric_limits<uint32_t>::max() - _sourceIndexData.size()) / 3)
        throw std::runtime_error("Vulkan: source geometry pool exceeds shader index range");
    auto &location = it->second;
    location.vertexOffset = static_cast<uint32_t>(_sourceVertexData.size());
    location.indexOffset = static_cast<uint32_t>(_sourceIndexData.size());
    location.vertexDataCount = static_cast<uint32_t>(vertices.size());
    location.indexCount = static_cast<uint32_t>(faces.size() * 3);
    _sourceVertexData.insert(_sourceVertexData.end(), vertices.begin(), vertices.end());
    for (const auto &face : faces) {
        _sourceIndexData.push_back(face.vertices[0]);
        _sourceIndexData.push_back(face.vertices[1]);
        _sourceIndexData.push_back(face.vertices[2]);
    }
    const auto vertexCapacity = grownCapacity(
        _sourceVertexCapacity, static_cast<uint32_t>(_sourceVertexData.size()), 256 * 1024);
    const auto indexCapacity = grownCapacity(
        _sourceIndexCapacity, static_cast<uint32_t>(_sourceIndexData.size()), 256 * 1024);
    if (!_sourceVertices || !_sourceIndices || vertexCapacity != _sourceVertexCapacity ||
        indexCapacity != _sourceIndexCapacity) {
        auto vertexBuffer = std::make_unique<VulkanBuffer>(_renderer->device());
        auto indexBuffer = std::make_unique<VulkanBuffer>(_renderer->device());
        vertexBuffer->initDeviceLocal(static_cast<VkDeviceSize>(vertexCapacity) * sizeof(float),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr);
        indexBuffer->initDeviceLocal(static_cast<VkDeviceSize>(indexCapacity) * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr);
        vertexBuffer->uploadDeviceLocal(
            0, static_cast<VkDeviceSize>(_sourceVertexData.size()) * sizeof(float),
            _sourceVertexData.data());
        indexBuffer->uploadDeviceLocal(
            0, static_cast<VkDeviceSize>(_sourceIndexData.size()) * sizeof(uint32_t),
            _sourceIndexData.data());
        if (_sourceVertices)
            _retiredSourceBuffers.push_back(std::move(_sourceVertices));
        if (_sourceIndices)
            _retiredSourceBuffers.push_back(std::move(_sourceIndices));
        _sourceVertices = std::move(vertexBuffer);
        _sourceIndices = std::move(indexBuffer);
        _sourceVertexCapacity = vertexCapacity;
        _sourceIndexCapacity = indexCapacity;
    } else {
        _sourceVertices->uploadDeviceLocal(
            static_cast<VkDeviceSize>(location.vertexOffset) * sizeof(float),
            static_cast<VkDeviceSize>(vertices.size()) * sizeof(float), vertices.data());
        _sourceIndices->uploadDeviceLocal(
            static_cast<VkDeviceSize>(location.indexOffset) * sizeof(uint32_t),
            static_cast<VkDeviceSize>(faces.size()) * 3 * sizeof(uint32_t),
            _sourceIndexData.data() + location.indexOffset);
    }
    return location;
}

VulkanGpuScene::View VulkanGpuScene::update(VkCommandBuffer cmd, GpuSceneUpload &upload) {
    R_PROFILE_ZONE("VulkanGpuScene::update");
    const auto resourceGeneration = _renderer->resources().generation();
    if (resourceGeneration != _sourceResourceGeneration) {
        clearSourceGeometry();
        _sourceResourceGeneration = resourceGeneration;
        if (++_sceneScope == 0)
            ++_sceneScope;
    }
    if (++_revision == 0)
        ++_revision;

    View empty;
    empty.sceneScope = _sceneScope;
    empty.revision = _revision;
    if (upload.objects.empty())
        return empty;

    auto &frame = *_frames[_renderer->frameIndex()];
    if (!_grassFaces || _grassFaceGeneration != upload.grassFaceGeneration) {
        auto grassFaces = std::make_unique<VulkanBuffer>(_renderer->device());
        const GpuSceneGrassFace emptyFace {};
        const VkDeviceSize faceCount = std::max<size_t>(1, upload.grassFaces.size());
        grassFaces->initDeviceLocal(
            faceCount * sizeof(GpuSceneGrassFace), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            upload.grassFaces.empty() ? static_cast<const void *>(&emptyFace)
                                      : static_cast<const void *>(upload.grassFaces.data()));
        if (_grassFaces)
            _retiredSourceBuffers.push_back(std::move(_grassFaces));
        _grassFaces = std::move(grassFaces);
        _grassFaceGeneration = upload.grassFaceGeneration;
    }
    uint64_t vertexCount = 0;
    uint64_t opaqueTriangleCount = 0;
    uint64_t nonOpaqueTriangleCount = 0;
    for (auto &input : upload.objects) {
        auto &object = input.data;
        if (input.sourceMesh) {
            _renderer->resources().get(*input.sourceMesh);
            const auto &source = appendSourceGeometry(*input.sourceMesh);
            const auto &layout = input.sourceMesh->vertexLayout();
            if (layout.stride % sizeof(float) != 0 ||
                layout.offPosition % static_cast<int>(sizeof(float)) != 0 ||
                (layout.offNormals >= 0 && layout.offNormals % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offUV1 >= 0 && layout.offUV1 % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offUV2 >= 0 && layout.offUV2 % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offTanSpace >= 0 && layout.offTanSpace % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offBoneIndices >= 0 && layout.offBoneIndices % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offBoneWeights >= 0 && layout.offBoneWeights % static_cast<int>(sizeof(float)) != 0))
                throw std::runtime_error("Vulkan: source vertex attributes must be float-aligned");
            object.srcVertexOffset = source.vertexOffset;
            object.srcIndexOffset = source.indexOffset;
            object.srcVertexStride = static_cast<uint32_t>(layout.stride);
            object.offPosition = layout.offPosition;
            object.offNormals = layout.offNormals;
            object.offUV1 = layout.offUV1;
            object.offUV2 = layout.offUV2;
            object.offTanSpace = layout.offTanSpace;
            object.offBoneIndices = layout.offBoneIndices;
            object.offBoneWeights = layout.offBoneWeights;
            object.vertexCount = static_cast<uint32_t>(input.sourceMesh->vertexCount());
            object.triangleCount = static_cast<uint32_t>(input.sourceMesh->faces().size());
        }
        if (vertexCount + object.vertexCount > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Vulkan: merged scene exceeds shader index range");
        auto &triangleBase = object.geometryIndex == 0 ? opaqueTriangleCount : nonOpaqueTriangleCount;
        if (triangleBase + object.triangleCount > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Vulkan: merged scene exceeds shader index range");
        object.dstVertexBase = static_cast<uint32_t>(vertexCount);
        object.dstTriangleBase = static_cast<uint32_t>(triangleBase);
        vertexCount += object.vertexCount;
        triangleBase += object.triangleCount;
    }
    const uint64_t triangleCount = opaqueTriangleCount + nonOpaqueTriangleCount;
    if (vertexCount == 0 || triangleCount == 0)
        return empty;
    if (upload.objects.size() > std::numeric_limits<uint32_t>::max() ||
        upload.bones.size() > std::numeric_limits<uint32_t>::max() ||
        upload.danglyPositions.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Vulkan: merged scene exceeds shader index range");

    VkDeviceSize sceneObjectBytes = 0;
    VkDeviceSize sceneBoneBytes = 0;
    VkDeviceSize danglyPositionBytes = 0;
    VkDeviceSize vertexBytes = 0;
    VkDeviceSize indexBytes = 0;
    {
        R_PROFILE_ZONE("VulkanGpuScene::staging upload");
        ensureMergeBuffers(frame, static_cast<uint32_t>(upload.objects.size()),
                           static_cast<uint32_t>(upload.bones.size()),
                           static_cast<uint32_t>(vertexCount), static_cast<uint32_t>(triangleCount),
                           static_cast<uint32_t>(upload.proceduralQuads.size()),
                           static_cast<uint32_t>(upload.danglyPositions.size()),
                           static_cast<uint32_t>(upload.grassRanges.size()));
        sceneObjectBytes =
            static_cast<VkDeviceSize>(frame.sceneObjectCapacity) * sizeof(SceneObject);
        sceneBoneBytes =
            static_cast<VkDeviceSize>(frame.boneCapacity) * sizeof(GpuSceneMatrix3x4);
        danglyPositionBytes =
            static_cast<VkDeviceSize>(frame.danglyPositionCapacity) * sizeof(glm::vec4);
        vertexBytes =
            static_cast<VkDeviceSize>(frame.vertexCapacity) * sizeof(MergedVertex);
        indexBytes =
            static_cast<VkDeviceSize>(frame.triangleCapacity) * 3 * sizeof(uint32_t);
        auto *sceneObjects = static_cast<SceneObject *>(frame.scene->mapped());
        for (size_t i = 0; i < upload.objects.size(); ++i)
            sceneObjects[i] = upload.objects[i].data;
        if (!upload.bones.empty())
            std::memcpy(static_cast<std::byte *>(frame.scene->mapped()) + sceneObjectBytes,
                        upload.bones.data(), upload.bones.size() * sizeof(GpuSceneMatrix3x4));
        if (!upload.danglyPositions.empty())
            std::memcpy(static_cast<std::byte *>(frame.scene->mapped()) + sceneObjectBytes + sceneBoneBytes,
                        upload.danglyPositions.data(), upload.danglyPositions.size() * sizeof(glm::vec4));
        if (!upload.proceduralQuads.empty())
            std::memcpy(frame.proceduralQuads->mapped(), upload.proceduralQuads.data(),
                        upload.proceduralQuads.size() * sizeof(GpuSceneProceduralQuad));
        if (!upload.grassRanges.empty())
            std::memcpy(frame.grassRanges->mapped(), upload.grassRanges.data(),
                        upload.grassRanges.size() * sizeof(GpuSceneGrassRange));

        frame.materials = std::make_unique<VulkanBuffer>(_renderer->device());
        frame.materials->initHostVisible(
            static_cast<VkDeviceSize>(upload.materials.size()) * sizeof(InstanceMaterial),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(frame.materials->mapped(), upload.materials.data(),
                    upload.materials.size() * sizeof(InstanceMaterial));
    }

    {
        R_PROFILE_ZONE("VulkanGpuScene::command recording");
        const auto *sourceVertices =
            _sourceVertices ? _sourceVertices.get() : frame.proceduralQuads.get();
        const auto *sourceIndices =
            _sourceIndices ? _sourceIndices.get() : frame.proceduralQuads.get();
        std::array<VkDescriptorBufferInfo, 11> buffers {{{frame.scene->handle(), 0, sceneObjectBytes},
                                                        {frame.scene->handle(), sceneObjectBytes, sceneBoneBytes},
                                                        {frame.geometry->handle(), 0, vertexBytes},
                                                        {frame.geometry->handle(), vertexBytes, indexBytes},
                                                        {frame.geometry->handle(), vertexBytes + indexBytes,
                                                         static_cast<VkDeviceSize>(frame.triangleCapacity) * sizeof(uint32_t)},
                                                        {sourceVertices->handle(), 0, sourceVertices->size()},
                                                        {sourceIndices->handle(), 0, sourceIndices->size()},
                                                        {frame.proceduralQuads->handle(), 0, frame.proceduralQuads->size()},
                                                        {frame.scene->handle(), sceneObjectBytes + sceneBoneBytes,
                                                         danglyPositionBytes},
                                                        {_grassFaces->handle(), 0, _grassFaces->size()},
                                                        {frame.grassRanges->handle(), 0, frame.grassRanges->size()}}};
        std::array<VkWriteDescriptorSet, 11> writes {};
        const auto set = _mergeSets[_renderer->frameIndex()];
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i];
        }
        vkUpdateDescriptorSets(_renderer->device().handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        MergePushConstants constants;
        constants.objectCount = static_cast<uint32_t>(upload.objects.size());
        constants.opaqueObjectCount = upload.opaqueObjectCount;
        constants.vertexCount = static_cast<uint32_t>(vertexCount);
        constants.triangleCount = static_cast<uint32_t>(triangleCount);
        constants.opaqueTriangleCount = static_cast<uint32_t>(opaqueTriangleCount);
        constants.cameraPosition = upload.cameraPosition;
        std::array<VkBufferMemoryBarrier2, 2> sourceBarriers {};
        for (size_t i = 0; i < sourceBarriers.size(); ++i) {
            auto &barrier = sourceBarriers[i];
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.buffer = buffers[5 + i].buffer;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
        }
        VkDependencyInfo sourceDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        sourceDependency.bufferMemoryBarrierCount = static_cast<uint32_t>(sourceBarriers.size());
        sourceDependency.pBufferMemoryBarriers = sourceBarriers.data();
        vkCmdPipelineBarrier2(cmd, &sourceDependency);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _mergePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _mergePipelineLayout, 0, 1,
                                &set, 0, nullptr);
        vkCmdPushConstants(cmd, _mergePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(constants), &constants);
        const auto threads = std::max(constants.vertexCount, constants.triangleCount);
        if (threads)
            vkCmdDispatch(cmd, (threads + 63) / 64, 1, 1);
        VkMemoryBarrier2 mergeBarrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mergeBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mergeBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        // The tracer is no longer the only consumer: F2 makes raster read the same
        // merged buffer, pulling vertices in the vertex shader and binding the
        // index range as indices. Both stages have to be named here or the raster
        // draw races the merge compute - and it would race silently, because the
        // previous frame's contents are usually close enough to look right.
        mergeBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR |
                                    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        mergeBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                     VK_ACCESS_2_SHADER_READ_BIT |
                                     VK_ACCESS_2_INDEX_READ_BIT;
        VkDependencyInfo mergeDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        mergeDependency.memoryBarrierCount = 1;
        mergeDependency.pMemoryBarriers = &mergeBarrier;
        vkCmdPipelineBarrier2(cmd, &mergeDependency);
    }

    const VkDeviceSize writtenVertexBytes =
        static_cast<VkDeviceSize>(vertexCount) * sizeof(MergedVertex);
    const VkDeviceSize writtenIndexBytes =
        static_cast<VkDeviceSize>(triangleCount) * 3 * sizeof(uint32_t);
    const VkDeviceSize writtenMaterialIdBytes =
        static_cast<VkDeviceSize>(triangleCount) * sizeof(uint32_t);
    frame.primitiveIds.resize(upload.objects.size());
    for (size_t objectIndex = 0; objectIndex < upload.objects.size(); ++objectIndex) {
        const auto &input = upload.objects[objectIndex];
        const auto &object = input.data;
        const uint32_t first =
            (object.geometryIndex == 0 ? 0 : static_cast<uint32_t>(opaqueTriangleCount)) +
            object.dstTriangleBase;
        frame.primitiveIds[objectIndex] =
            {first, object.triangleCount,
             {_sceneScope, input.objectIndex, input.objectGeneration, 0}};
    }
    View view;
    view.sceneScope = _sceneScope;
    view.revision = _revision;
    view.vertices = {frame.geometry.get(), 0, writtenVertexBytes};
    view.indices = {frame.geometry.get(), vertexBytes, writtenIndexBytes};
    view.materialIds =
        {frame.geometry.get(), vertexBytes + indexBytes, writtenMaterialIdBytes};
    view.materials = {frame.materials.get(), 0, frame.materials->size()};
    view.objectCount = static_cast<uint32_t>(upload.objects.size());
    view.opaqueObjectCount = upload.opaqueObjectCount;
    view.vertexCount = static_cast<uint32_t>(vertexCount);
    view.opaqueTriangleCount = static_cast<uint32_t>(opaqueTriangleCount);
    view.triangleCount = static_cast<uint32_t>(triangleCount);
    view.primitiveIds =
        {frame.primitiveIds.data(), static_cast<uint32_t>(frame.primitiveIds.size())};
    view.regions = {{GpuSceneResidencyClass::Dynamic, _revision, 0,
                     static_cast<uint32_t>(vertexCount), 0,
                     static_cast<uint32_t>(triangleCount)}};
    return view;
}

} // namespace reone::graphics
