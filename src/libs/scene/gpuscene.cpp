/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/scene/gpuscene.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "reone/graphics/mesh.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/scene/registry.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone::scene {
namespace {
struct MergePushConstants {
    uint32_t objectCount;
    uint32_t opaqueObjectCount;
    uint32_t vertexCount;
    uint32_t triangleCount;
    uint32_t opaqueTriangleCount;
};

GpuScene::Matrix3x4 matrix3x4(const glm::mat4 &m) {
    return {{m[0][0], m[1][0], m[2][0], m[3][0]},
            {m[0][1], m[1][1], m[2][1], m[3][1]},
            {m[0][2], m[1][2], m[2][2], m[3][2]}};
}

uint32_t grownCapacity(uint32_t current, uint32_t required, uint32_t minimum) {
    if (current != 0 && required <= current) return current;
    uint64_t capacity = std::max(current, minimum);
    while (capacity < required) capacity *= 2;
    if (capacity > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Vulkan: merged scene capacity exceeds uint32 range");
    return static_cast<uint32_t>(capacity);
}
} // namespace

struct GpuScene::Frame {
    std::unique_ptr<VulkanBuffer> scene;
    std::unique_ptr<VulkanBuffer> geometry;
    std::unique_ptr<VulkanBuffer> grassClusters;
    std::unique_ptr<VulkanBuffer> materials;
    uint32_t sceneObjectCapacity {0};
    uint32_t boneCapacity {0};
    uint32_t danglyPositionCapacity {0};
    uint32_t vertexCapacity {0};
    uint32_t triangleCapacity {0};
    uint32_t grassClusterCapacity {0};
    std::vector<PrimitiveIdRange> primitiveIds;
};

GpuScene::GpuScene(VulkanRenderer &renderer) : _renderer(renderer) {}
GpuScene::~GpuScene() { deinit(); }

GpuScene::PrimitiveId GpuScene::PrimitiveIdView::operator[](uint32_t index) const {
    for (uint32_t rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex) {
        const auto &range = ranges[rangeIndex];
        if (index < range.firstTriangle || index - range.firstTriangle >= range.triangleCount) continue;
        auto id = range.first;
        id.localPrimitive += index - range.firstTriangle;
        return id;
    }
    throw std::out_of_range("Vulkan: frame-local primitive address is not published");
}

void GpuScene::init() {
    if (_inited) return;
    auto &device = _renderer.device();
    VkDescriptorSetLayoutBinding bindings[9] {};
    for (uint32_t i = 0; i < 9; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 9;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device.handle(), &layoutInfo, nullptr, &_mergeLayout) != VK_SUCCESS)
        throw std::runtime_error("Vulkan: merge descriptor layout creation failed");
    VkDescriptorPoolSize poolSize {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 18};
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
    auto spirv = readSpirV(_renderer.shaderDir() / "skin.spv");
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
    if (vkCreatePipelineLayout(device.handle(), &pipelineLayoutInfo, nullptr, &_mergePipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: merge pipeline layout creation failed");
    }
    VkComputePipelineCreateInfo pipelineInfo {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = _mergePipelineLayout;
    const auto result = vkCreateComputePipelines(device.handle(), VK_NULL_HANDLE, 1, &pipelineInfo,
                                                 nullptr, &_mergePipeline);
    vkDestroyShaderModule(device.handle(), module, nullptr);
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan: merge compute pipeline creation failed");
    device.setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(_mergePipeline), "gpu-scene:merge");
    for (auto &frame : _frames) frame = std::make_unique<Frame>();
    _inited = true;
}

void GpuScene::deinit() {
    if (!_inited) return;
    auto &device = _renderer.device();
    _frames = {};
    clearSourceGeometry();
    if (_mergePipeline) vkDestroyPipeline(device.handle(), _mergePipeline, nullptr);
    if (_mergePipelineLayout) vkDestroyPipelineLayout(device.handle(), _mergePipelineLayout, nullptr);
    if (_mergePool) vkDestroyDescriptorPool(device.handle(), _mergePool, nullptr);
    if (_mergeLayout) vkDestroyDescriptorSetLayout(device.handle(), _mergeLayout, nullptr);
    _mergePipeline = VK_NULL_HANDLE;
    _mergePipelineLayout = VK_NULL_HANDLE;
    _mergePool = VK_NULL_HANDLE;
    _mergeLayout = VK_NULL_HANDLE;
    _mergeSets = {};
    _inited = false;
}

void GpuScene::clearSourceGeometry() {
    _sourceGeometry.clear();
    _sourceVertexData.clear();
    _sourceIndexData.clear();
    _sourceVertices.reset();
    _sourceIndices.reset();
    _retiredSourceBuffers.clear();
    _sourceVertexCapacity = 0;
    _sourceIndexCapacity = 0;
}

void GpuScene::ensureMergeBuffers(Frame &frame, uint32_t objectCount, uint32_t boneCount,
                                  uint32_t vertexCount, uint32_t triangleCount,
                                  uint32_t grassClusterCount, uint32_t danglyPositionCount) {
    constexpr uint32_t kInitialObjectCapacity = 64, kInitialBoneCapacity = 256;
    constexpr uint32_t kInitialVertexCapacity = 4096, kInitialTriangleCapacity = 4096;
    const auto objectCapacity = grownCapacity(frame.sceneObjectCapacity, objectCount, kInitialObjectCapacity);
    const auto boneCapacity = grownCapacity(frame.boneCapacity, boneCount, kInitialBoneCapacity);
    const auto danglyPositionCapacity = grownCapacity(frame.danglyPositionCapacity, danglyPositionCount, 256);
    if (!frame.scene || objectCapacity != frame.sceneObjectCapacity || boneCapacity != frame.boneCapacity ||
        danglyPositionCapacity != frame.danglyPositionCapacity) {
        frame.sceneObjectCapacity = objectCapacity;
        frame.boneCapacity = boneCapacity;
        frame.danglyPositionCapacity = danglyPositionCapacity;
        frame.scene = std::make_unique<VulkanBuffer>(_renderer.device());
        frame.scene->initHostVisible(static_cast<VkDeviceSize>(objectCapacity) * sizeof(SceneObject) +
                                     static_cast<VkDeviceSize>(boneCapacity) * sizeof(Matrix3x4) +
                                     static_cast<VkDeviceSize>(danglyPositionCapacity) * sizeof(glm::vec4),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
    const auto vertexCapacity = grownCapacity(frame.vertexCapacity, vertexCount, kInitialVertexCapacity);
    const auto triangleCapacity = grownCapacity(frame.triangleCapacity, triangleCount, kInitialTriangleCapacity);
    if (!frame.geometry || vertexCapacity != frame.vertexCapacity || triangleCapacity != frame.triangleCapacity) {
        frame.vertexCapacity = vertexCapacity;
        frame.triangleCapacity = triangleCapacity;
        const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertexCapacity) * sizeof(MergedVertex);
        const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(triangleCapacity) * 3 * sizeof(uint32_t);
        const VkDeviceSize materialIdBytes = static_cast<VkDeviceSize>(triangleCapacity) * sizeof(uint32_t);
        frame.geometry = std::make_unique<VulkanBuffer>(_renderer.device());
        frame.geometry->initDeviceLocal(vertexBytes + indexBytes + materialIdBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, nullptr);
    }
    const auto grassClusterCapacity = grownCapacity(frame.grassClusterCapacity, grassClusterCount, 64);
    if (!frame.grassClusters || grassClusterCapacity != frame.grassClusterCapacity) {
        frame.grassClusterCapacity = grassClusterCapacity;
        frame.grassClusters = std::make_unique<VulkanBuffer>(_renderer.device());
        frame.grassClusters->initHostVisible(
            // ProceduralQuad is six vec4s (the last two carry lightmap UV and
            // per-quad colour); keep this in lockstep with skin.slang.
            static_cast<VkDeviceSize>(grassClusterCapacity) * sizeof(glm::vec4) * 6,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }
}

const GpuScene::SourceGeometry &GpuScene::appendSourceGeometry(const Mesh &mesh) {
    const auto [it, inserted] = _sourceGeometry.emplace(&mesh, SourceGeometry {});
    const auto &vertices = mesh.vertexData();
    const auto &faces = mesh.faces();
    if (!inserted) {
        // Instrument the raw-pointer cache while validating its module-transition
        // lifetime. A mismatch means this address was recycled for a new Mesh.
        if (it->second.vertexDataCount != vertices.size() || it->second.indexCount != faces.size() * 3) {
            warn("Vulkan: GpuScene source cache reused a Mesh address with different geometry", LogChannel::Graphics);
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
    const auto grow = [](uint32_t current, uint32_t required, uint32_t minimum) {
        return grownCapacity(current, required, minimum);
    };
    const auto vertexCapacity = grow(_sourceVertexCapacity, static_cast<uint32_t>(_sourceVertexData.size()), 256 * 1024);
    const auto indexCapacity = grow(_sourceIndexCapacity, static_cast<uint32_t>(_sourceIndexData.size()), 256 * 1024);
    if (!_sourceVertices || !_sourceIndices || vertexCapacity != _sourceVertexCapacity || indexCapacity != _sourceIndexCapacity) {
        auto vertexBuffer = std::make_unique<VulkanBuffer>(_renderer.device());
        auto indexBuffer = std::make_unique<VulkanBuffer>(_renderer.device());
        vertexBuffer->initDeviceLocal(static_cast<VkDeviceSize>(vertexCapacity) * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr);
        indexBuffer->initDeviceLocal(static_cast<VkDeviceSize>(indexCapacity) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, nullptr);
        vertexBuffer->uploadDeviceLocal(0, static_cast<VkDeviceSize>(_sourceVertexData.size()) * sizeof(float), _sourceVertexData.data());
        indexBuffer->uploadDeviceLocal(0, static_cast<VkDeviceSize>(_sourceIndexData.size()) * sizeof(uint32_t), _sourceIndexData.data());
        if (_sourceVertices) _retiredSourceBuffers.push_back(std::move(_sourceVertices));
        if (_sourceIndices) _retiredSourceBuffers.push_back(std::move(_sourceIndices));
        _sourceVertices = std::move(vertexBuffer);
        _sourceIndices = std::move(indexBuffer);
        _sourceVertexCapacity = vertexCapacity;
        _sourceIndexCapacity = indexCapacity;
    } else {
        _sourceVertices->uploadDeviceLocal(static_cast<VkDeviceSize>(location.vertexOffset) * sizeof(float),
                                           static_cast<VkDeviceSize>(vertices.size()) * sizeof(float), vertices.data());
        _sourceIndices->uploadDeviceLocal(static_cast<VkDeviceSize>(location.indexOffset) * sizeof(uint32_t),
                                          static_cast<VkDeviceSize>(faces.size()) * 3 * sizeof(uint32_t),
                                          _sourceIndexData.data() + location.indexOffset);
    }
    return location;
}

GpuScene::View GpuScene::update(VkCommandBuffer cmd,
                                RenderRegistry &registry,
                                const Classifier &classifier,
                                const GrassClassifier &grassClassifier,
                                const ParticleClassifier &particleClassifier,
                                const BillboardClassifier &billboardClassifier,
                                const glm::mat4 &cameraView) {
    const auto resourceGeneration = _renderer.resources().generation();
    if (resourceGeneration != _sourceResourceGeneration) {
        // invalidateResources() has waited for the GPU and discarded the
        // module-owned Mesh cache. Mesh addresses are therefore no longer
        // identity-safe: clear the source pool before the next module can
        // reuse an address with unrelated geometry.
        clearSourceGeometry();
        _sourceResourceGeneration = resourceGeneration;
        if (++_sceneScope == 0) ++_sceneScope;
    }
    if (++_revision == 0) ++_revision;
    auto &frame = *_frames[_renderer.frameIndex()];
    std::vector<InstanceMaterial> materials;
    std::vector<SceneObject> opaqueObjects, nonOpaqueObjects;
    std::vector<SceneNodeId> opaqueObjectIds, nonOpaqueObjectIds;
    std::vector<Matrix3x4> bones;
    std::vector<glm::vec4> danglyPositions;
    struct alignas(16) ProceduralQuad {
        glm::vec4 positionVariant {0.0f};
        glm::vec4 right {0.0f};
        glm::vec4 up {0.0f};
        glm::vec4 uvOffsetScale {0.0f, 0.0f, 1.0f, 1.0f};
        glm::vec2 lightmapUV {0.0f};
        glm::vec2 pad {0.0f};
        glm::vec4 color {1.0f};
    };
    static_assert(sizeof(ProceduralQuad) == sizeof(glm::vec4) * 6);
    std::vector<ProceduralQuad> proceduralQuads;
    materials.reserve(registry.objects().size());
    opaqueObjects.reserve(registry.objects().size());
    nonOpaqueObjects.reserve(registry.objects().size());
    opaqueObjectIds.reserve(registry.objects().size());
    nonOpaqueObjectIds.reserve(registry.objects().size());
    const glm::vec3 cameraPosition = glm::vec3(glm::inverse(cameraView)[3]);
    const glm::vec3 viewRow0 = glm::vec3(cameraView[0]);
    const glm::vec3 viewRow1 = glm::vec3(cameraView[1]);
    const glm::vec3 viewRow2 = glm::vec3(cameraView[2]);
    for (const auto &object : registry.objects()) {
        const auto *mesh = std::get_if<RegisteredMesh>(&object);
        if (mesh) {
            if (!registry.isObjectEnabled(mesh->id.index)) continue;
            if ((mesh->categories & (renderCategory(RenderCategory::Opaque) | renderCategory(RenderCategory::Transparent))) == 0) continue;
            auto admission = classifier(*mesh);
            if (!admission) continue;
            _renderer.resources().get(mesh->mesh.get());
            const auto &source = appendSourceGeometry(mesh->mesh.get());
            const auto &layout = mesh->mesh.get().vertexLayout();
            if (layout.stride % sizeof(float) != 0 || layout.offPosition % static_cast<int>(sizeof(float)) != 0 ||
                (layout.offNormals >= 0 && layout.offNormals % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offUV1 >= 0 && layout.offUV1 % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offUV2 >= 0 && layout.offUV2 % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offTanSpace >= 0 && layout.offTanSpace % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offBoneIndices >= 0 && layout.offBoneIndices % static_cast<int>(sizeof(float)) != 0) ||
                (layout.offBoneWeights >= 0 && layout.offBoneWeights % static_cast<int>(sizeof(float)) != 0))
                throw std::runtime_error("Vulkan: source vertex attributes must be float-aligned");
            SceneObject sceneObject;
            sceneObject.transform = matrix3x4(mesh->transform);
            sceneObject.prevTransform = matrix3x4(mesh->prevTransform);
            sceneObject.srcVertexOffset = source.vertexOffset;
            sceneObject.srcIndexOffset = source.indexOffset;
            sceneObject.srcVertexStride = static_cast<uint32_t>(layout.stride);
            sceneObject.offPosition = layout.offPosition; sceneObject.offNormals = layout.offNormals;
            sceneObject.offUV1 = layout.offUV1; sceneObject.offUV2 = layout.offUV2;
            sceneObject.offTanSpace = layout.offTanSpace; sceneObject.offBoneIndices = layout.offBoneIndices;
            sceneObject.offBoneWeights = layout.offBoneWeights;
            sceneObject.vertexCount = static_cast<uint32_t>(mesh->mesh.get().vertexCount());
            sceneObject.triangleCount = static_cast<uint32_t>(mesh->mesh.get().faces().size());
            sceneObject.materialIndex = static_cast<uint32_t>(materials.size());
            if (admission->skin) {
                const auto &skin = *admission->skin;
                if (skin.bones.size() != skin.prevBones.size())
                    throw std::runtime_error("Vulkan: skinned mesh has mismatched bone palettes");
                if (bones.size() + skin.bones.size() + skin.prevBones.size() > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Vulkan: merged scene exceeds shader index range");
                sceneObject.boneBase = static_cast<uint32_t>(bones.size());
                sceneObject.boneCount = static_cast<uint32_t>(skin.bones.size());
                for (const auto &bone : skin.bones) bones.push_back(matrix3x4(bone));
                for (const auto &bone : skin.prevBones) bones.push_back(matrix3x4(bone));
            }
            if (const auto *dangly = std::get_if<RegisteredDangly>(&mesh->deformation)) {
                if (dangly->positions.size() != sceneObject.vertexCount ||
                    dangly->prevPositions.size() != sceneObject.vertexCount)
                    throw std::runtime_error("Vulkan: dangly mesh has mismatched position streams");
                if (danglyPositions.size() + dangly->positions.size() + dangly->prevPositions.size() >
                    std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Vulkan: merged dangly position pool exceeds shader index range");
                sceneObject.danglyBase = static_cast<uint32_t>(danglyPositions.size());
                sceneObject.danglyCount = sceneObject.vertexCount;
                danglyPositions.insert(danglyPositions.end(), dangly->positions.begin(), dangly->positions.end());
                danglyPositions.insert(danglyPositions.end(), dangly->prevPositions.begin(), dangly->prevPositions.end());
            }
            if (const auto *saber = std::get_if<RegisteredSaber>(&mesh->deformation)) {
                sceneObject.saberDisplacement = saber->displacement;
            }
            sceneObject.geometryIndex = admission->primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
            materials.push_back(admission->material);
            if (sceneObject.geometryIndex == 0) {
                opaqueObjects.push_back(sceneObject);
                opaqueObjectIds.push_back(mesh->id);
            } else {
                nonOpaqueObjects.push_back(sceneObject);
                nonOpaqueObjectIds.push_back(mesh->id);
            }
            continue;
        }
        const auto *grass = std::get_if<RegisteredGrass>(&object);
        if (!grass || !registry.isObjectEnabled(grass->id.index) || grass->instances.empty()) continue;
        if ((grass->categories & (renderCategory(RenderCategory::Opaque) | renderCategory(RenderCategory::Transparent))) == 0) continue;
        auto admission = grassClassifier(*grass);
        if (!admission) continue;
        if (grass->instances.size() > std::numeric_limits<uint32_t>::max() / 4 ||
            proceduralQuads.size() > std::numeric_limits<uint32_t>::max() - grass->instances.size())
            throw std::runtime_error("Vulkan: merged grass scene exceeds shader index range");
        SceneObject sceneObject;
        sceneObject.srcVertexOffset = static_cast<uint32_t>(proceduralQuads.size());
        // A zero source stride tags a procedural grass-cluster list. The merge
        // shader expands it directly, avoiding one CPU SceneObject per blade.
        sceneObject.srcVertexStride = 0;
        sceneObject.vertexCount = static_cast<uint32_t>(grass->instances.size()) * 4;
        sceneObject.triangleCount = static_cast<uint32_t>(grass->instances.size()) * 2;
        sceneObject.materialIndex = static_cast<uint32_t>(materials.size());
        sceneObject.geometryIndex = admission->primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
        for (const auto &instance : grass->instances) {
            const float angle = glm::asin(glm::smoothstep(0.5f * grass->radius, grass->radius,
                                                          glm::distance(instance.position, cameraPosition)));
            // This uses the primary camera's billboard axes at merge time. A
            // bounce ray has a different ideal pose, but that small orientation
            // error is preferable to grass being absent from the BLAS entirely.
            const glm::vec3 right = viewRow0 * grass->quadSize;
            const glm::vec3 up = (glm::cos(angle) * viewRow1 - glm::sin(angle) * viewRow2) * grass->quadSize;
            const glm::vec2 uvOffset {0.5f * (instance.variant % 2),
                                      0.5f * (instance.variant / 2)};
            proceduralQuads.push_back({glm::vec4(instance.position, static_cast<float>(instance.variant)),
                                       glm::vec4(right, 0.0f), glm::vec4(up, 0.0f),
                                       glm::vec4(uvOffset, glm::vec2(0.5f)), instance.lightmapUV,
                                       {0.0f, 0.0f}, glm::vec4(1.0f)});
        }
        materials.push_back(admission->material);
        if (sceneObject.geometryIndex == 0) {
            opaqueObjects.push_back(sceneObject);
            opaqueObjectIds.push_back(grass->id);
        } else {
            nonOpaqueObjects.push_back(sceneObject);
            nonOpaqueObjectIds.push_back(grass->id);
        }
    }
    for (const auto &object : registry.objects()) {
        const auto *particles = std::get_if<RegisteredParticles>(&object);
        if (!particles || !registry.isObjectEnabled(particles->id.index) || particles->instances.empty()) continue;
        if ((particles->categories & (renderCategory(RenderCategory::Opaque) |
                                      renderCategory(RenderCategory::Transparent))) == 0) continue;
        auto admission = particleClassifier(*particles);
        if (!admission) continue;
        if (particles->instances.size() > std::numeric_limits<uint32_t>::max() / 4 ||
            proceduralQuads.size() > std::numeric_limits<uint32_t>::max() - particles->instances.size())
            throw std::runtime_error("Vulkan: merged particle scene exceeds shader index range");
        SceneObject sceneObject;
        sceneObject.srcVertexOffset = static_cast<uint32_t>(proceduralQuads.size());
        // One is the centered billboard tag. As with grass (zero), the merge
        // expands this compact emitter list into quads rather than receiving
        // one CPU SceneObject per particle.
        sceneObject.srcVertexStride = 1;
        sceneObject.vertexCount = static_cast<uint32_t>(particles->instances.size()) * 4;
        sceneObject.triangleCount = static_cast<uint32_t>(particles->instances.size()) * 2;
        sceneObject.materialIndex = static_cast<uint32_t>(materials.size());
        sceneObject.geometryIndex = admission->primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
        const glm::ivec2 grid = glm::max(particles->gridSize, glm::ivec2(1));
        for (const auto &instance : particles->instances) {
            const int frame = std::max(0, instance.frame);
            const glm::vec2 uvScale {1.0f / grid.x, 1.0f / grid.y};
            const glm::vec2 uvOffset {(frame % grid.x) * uvScale.x, (frame / grid.x) * uvScale.y};
            proceduralQuads.push_back({glm::vec4(instance.position, static_cast<float>(frame)),
                                       glm::vec4(instance.right * instance.size.x, 0.0f),
                                       glm::vec4(instance.up * instance.size.y, 0.0f),
                                       glm::vec4(uvOffset, uvScale), glm::vec2(0.0f), {0.0f, 0.0f},
                                       instance.color});
        }
        materials.push_back(admission->material);
        if (sceneObject.geometryIndex == 0) {
            opaqueObjects.push_back(sceneObject);
            opaqueObjectIds.push_back(particles->id);
        } else {
            nonOpaqueObjects.push_back(sceneObject);
            nonOpaqueObjectIds.push_back(particles->id);
        }
    }
    for (const auto &object : registry.objects()) {
        const auto *billboard = std::get_if<RegisteredBillboard>(&object);
        if (!billboard || !registry.isObjectEnabled(billboard->id.index)) continue;
        if ((billboard->categories & (renderCategory(RenderCategory::Opaque) |
                                      renderCategory(RenderCategory::Transparent))) == 0) continue;
        auto admission = billboardClassifier(*billboard);
        if (!admission) continue;
        SceneObject sceneObject;
        sceneObject.srcVertexOffset = static_cast<uint32_t>(proceduralQuads.size());
        sceneObject.srcVertexStride = 1;
        sceneObject.vertexCount = 4;
        sceneObject.triangleCount = 2;
        sceneObject.materialIndex = static_cast<uint32_t>(materials.size());
        sceneObject.geometryIndex = admission->primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
        // Billboard rasterization gets its axes from the primary camera. The
        // tracer bakes that same primary-camera approximation into the merged
        // quad; reflections and shadows therefore see a fixed pose.
        const float width = glm::length(glm::vec3(billboard->transform[0]));
        const float height = glm::length(glm::vec3(billboard->transform[1]));
        proceduralQuads.push_back({glm::vec4(glm::vec3(billboard->transform[3]), 0.0f),
                                   glm::vec4(viewRow0 * width, 0.0f), glm::vec4(viewRow1 * height, 0.0f),
                                   glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), glm::vec2(0.0f), {0.0f, 0.0f},
                                   billboard->color});
        materials.push_back(admission->material);
        if (sceneObject.geometryIndex == 0) {
            opaqueObjects.push_back(sceneObject);
            opaqueObjectIds.push_back(billboard->id);
        } else {
            nonOpaqueObjects.push_back(sceneObject);
            nonOpaqueObjectIds.push_back(billboard->id);
        }
    }
    std::vector<SceneObject> objects;
    std::vector<SceneNodeId> objectIds;
    objects.reserve(opaqueObjects.size() + nonOpaqueObjects.size());
    objectIds.reserve(opaqueObjectIds.size() + nonOpaqueObjectIds.size());
    objects.insert(objects.end(), opaqueObjects.begin(), opaqueObjects.end());
    objects.insert(objects.end(), nonOpaqueObjects.begin(), nonOpaqueObjects.end());
    objectIds.insert(objectIds.end(), opaqueObjectIds.begin(), opaqueObjectIds.end());
    objectIds.insert(objectIds.end(), nonOpaqueObjectIds.begin(), nonOpaqueObjectIds.end());
    uint64_t vertexCount = 0, opaqueTriangleCount = 0, nonOpaqueTriangleCount = 0;
    for (auto &object : objects) {
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
    if (objects.empty() || vertexCount == 0 || triangleCount == 0) {
        View empty;
        empty.sceneScope = _sceneScope;
        empty.revision = _revision;
        return empty;
    }
    if (objects.size() > std::numeric_limits<uint32_t>::max() || vertexCount > std::numeric_limits<uint32_t>::max() ||
        triangleCount > std::numeric_limits<uint32_t>::max() || opaqueTriangleCount > std::numeric_limits<uint32_t>::max() ||
        bones.size() > std::numeric_limits<uint32_t>::max() || danglyPositions.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("Vulkan: merged scene exceeds shader index range");
    ensureMergeBuffers(frame, static_cast<uint32_t>(objects.size()), static_cast<uint32_t>(bones.size()),
                       static_cast<uint32_t>(vertexCount), static_cast<uint32_t>(triangleCount),
                       static_cast<uint32_t>(proceduralQuads.size()), static_cast<uint32_t>(danglyPositions.size()));
    const VkDeviceSize sceneObjectBytes = static_cast<VkDeviceSize>(frame.sceneObjectCapacity) * sizeof(SceneObject);
    const VkDeviceSize sceneBoneBytes = static_cast<VkDeviceSize>(frame.boneCapacity) * sizeof(Matrix3x4);
    const VkDeviceSize danglyPositionBytes = static_cast<VkDeviceSize>(frame.danglyPositionCapacity) * sizeof(glm::vec4);
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(frame.vertexCapacity) * sizeof(MergedVertex);
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(frame.triangleCapacity) * 3 * sizeof(uint32_t);
    std::memcpy(frame.scene->mapped(), objects.data(), objects.size() * sizeof(SceneObject));
    if (!bones.empty()) std::memcpy(static_cast<std::byte *>(frame.scene->mapped()) + sceneObjectBytes,
                                    bones.data(), bones.size() * sizeof(Matrix3x4));
    if (!danglyPositions.empty()) std::memcpy(static_cast<std::byte *>(frame.scene->mapped()) + sceneObjectBytes + sceneBoneBytes,
                                              danglyPositions.data(), danglyPositions.size() * sizeof(glm::vec4));
    if (!proceduralQuads.empty()) std::memcpy(frame.grassClusters->mapped(), proceduralQuads.data(),
                                              proceduralQuads.size() * sizeof(ProceduralQuad));
    // A grass-only scene never appends mesh source data. Bind the procedural
    // cluster buffer to the otherwise-unused source slots in that case so the
    // complete descriptor set remains valid without inventing mesh records.
    const auto *sourceVertices = _sourceVertices ? _sourceVertices.get() : frame.grassClusters.get();
    const auto *sourceIndices = _sourceIndices ? _sourceIndices.get() : frame.grassClusters.get();
    std::array<VkDescriptorBufferInfo, 9> buffers {{{frame.scene->handle(), 0, sceneObjectBytes},
        {frame.scene->handle(), sceneObjectBytes, sceneBoneBytes}, {frame.geometry->handle(), 0, vertexBytes},
        {frame.geometry->handle(), vertexBytes, indexBytes}, {frame.geometry->handle(), vertexBytes + indexBytes,
        static_cast<VkDeviceSize>(frame.triangleCapacity) * sizeof(uint32_t)}, {sourceVertices->handle(), 0, sourceVertices->size()},
        {sourceIndices->handle(), 0, sourceIndices->size()}, {frame.grassClusters->handle(), 0, frame.grassClusters->size()},
        {frame.scene->handle(), sceneObjectBytes + sceneBoneBytes, danglyPositionBytes}}};
    std::array<VkWriteDescriptorSet, 9> writes {};
    const auto set = _mergeSets[_renderer.frameIndex()];
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set; writes[i].dstBinding = i;
        writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(_renderer.device().handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    const MergePushConstants constants {static_cast<uint32_t>(objects.size()), static_cast<uint32_t>(opaqueObjects.size()),
                                        static_cast<uint32_t>(vertexCount), static_cast<uint32_t>(triangleCount), static_cast<uint32_t>(opaqueTriangleCount)};
    std::array<VkBufferMemoryBarrier2, 2> sourceBarriers {};
    for (size_t i = 0; i < sourceBarriers.size(); ++i) {
        auto &barrier = sourceBarriers[i]; barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        barrier.buffer = buffers[5 + i].buffer; barrier.offset = 0; barrier.size = VK_WHOLE_SIZE;
    }
    VkDependencyInfo sourceDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    sourceDependency.bufferMemoryBarrierCount = static_cast<uint32_t>(sourceBarriers.size()); sourceDependency.pBufferMemoryBarriers = sourceBarriers.data();
    vkCmdPipelineBarrier2(cmd, &sourceDependency);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _mergePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _mergePipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, _mergePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    const auto threads = std::max(constants.vertexCount, constants.triangleCount);
    if (threads) vkCmdDispatch(cmd, (threads + 63) / 64, 1, 1);
    VkMemoryBarrier2 mergeBarrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mergeBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; mergeBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    mergeBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    mergeBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_SHADER_READ_BIT;
    VkDependencyInfo mergeDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; mergeDependency.memoryBarrierCount = 1; mergeDependency.pMemoryBarriers = &mergeBarrier;
    vkCmdPipelineBarrier2(cmd, &mergeDependency);
    frame.materials = std::make_unique<VulkanBuffer>(_renderer.device());
    frame.materials->initHostVisible(static_cast<VkDeviceSize>(materials.size()) * sizeof(InstanceMaterial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(frame.materials->mapped(), materials.data(), materials.size() * sizeof(InstanceMaterial));
    const VkDeviceSize writtenVertexBytes = static_cast<VkDeviceSize>(vertexCount) * sizeof(MergedVertex);
    const VkDeviceSize writtenIndexBytes = static_cast<VkDeviceSize>(triangleCount) * 3 * sizeof(uint32_t);
    const VkDeviceSize writtenMaterialIdBytes = static_cast<VkDeviceSize>(triangleCount) * sizeof(uint32_t);
    frame.primitiveIds.resize(objects.size());
    for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex) {
        const auto &object = objects[objectIndex];
        const uint32_t first = (object.geometryIndex == 0 ? 0 : static_cast<uint32_t>(opaqueTriangleCount)) +
                               object.dstTriangleBase;
        frame.primitiveIds[objectIndex] = {first, object.triangleCount, {_sceneScope, objectIds[objectIndex], 0}};
    }
    View view;
    view.sceneScope = _sceneScope;
    view.revision = _revision;
    view.vertices = {frame.geometry.get(), 0, writtenVertexBytes};
    view.indices = {frame.geometry.get(), vertexBytes, writtenIndexBytes};
    view.materialIds = {frame.geometry.get(), vertexBytes + indexBytes, writtenMaterialIdBytes};
    view.materials = {frame.materials.get(), 0, frame.materials->size()};
    view.objectCount = static_cast<uint32_t>(objects.size());
    view.opaqueObjectCount = static_cast<uint32_t>(opaqueObjects.size());
    view.vertexCount = static_cast<uint32_t>(vertexCount);
    view.opaqueTriangleCount = static_cast<uint32_t>(opaqueTriangleCount);
    view.triangleCount = static_cast<uint32_t>(triangleCount);
    view.primitiveIds = {frame.primitiveIds.data(), static_cast<uint32_t>(frame.primitiveIds.size())};
    // Rebuild-every-frame remains deliberately all-dynamic. Future retained
    // regions can use Admission::residency without changing this publication.
    view.regions = {{ResidencyClass::Dynamic, _revision, 0, static_cast<uint32_t>(vertexCount),
                     0, static_cast<uint32_t>(triangleCount)}};
    return view;
}
} // namespace reone::scene
