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
#include "reone/graphics/vulkan/tracingstructure.h"

#include "reone/graphics/rendering/gpuscene.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/rhi.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <vector>

namespace reone::graphics {
namespace {

VkDeviceAddress alignedAddress(VkDeviceAddress address, VkDeviceSize alignment) {
    return (address + alignment - 1) & ~(alignment - 1);
}

VkDeviceAddress accelerationStructureAddress(VulkanDevice &device,
                                             VkAccelerationStructureKHR structure) {
    VkAccelerationStructureDeviceAddressInfoKHR info {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    info.accelerationStructure = structure;
    return vkGetAccelerationStructureDeviceAddressKHR(device.handle(), &info);
}

/**
 * The template geometry for one grass-card variant.
 *
 * Every variant is padded to the same vertex and triangle count by the fitter,
 * so a variant is a fixed-size window into the shared template buffers rather
 * than a range that has to be carried alongside it.
 */
VkAccelerationStructureGeometryKHR cardGeometry(const SceneTracingGeometry &geometry,
                                                uint32_t variant) {
    VkAccelerationStructureGeometryTrianglesDataKHR triangles {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    triangles.vertexFormat = VK_FORMAT_R32G32_SFLOAT;
    triangles.vertexData.deviceAddress =
        toVulkanBuffer(*geometry.cardVertices.buffer).deviceAddress() +
        geometry.cardVertices.offset +
        static_cast<VkDeviceSize>(variant) * geometry.cardVertexCount * sizeof(glm::vec4);
    // The fitter stores a template vertex as (x, y, u, v), so position is the
    // first two lanes of a vec4 and the stride is the whole record.
    triangles.vertexStride = sizeof(glm::vec4);
    triangles.maxVertex = geometry.cardVertexCount - 1;
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress =
        toVulkanBuffer(*geometry.cardIndices.buffer).deviceAddress() +
        geometry.cardIndices.offset +
        static_cast<VkDeviceSize>(variant) * geometry.cardTriangleCount * 3 * sizeof(uint32_t);
    VkAccelerationStructureGeometryKHR result {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    result.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    result.geometry.triangles = triangles;
    // A card is a cutout: the fitted outline is conservative, so the ray-query
    // candidate loop still has to alpha-test what it encloses.
    result.flags = 0;
    return result;
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

std::unique_ptr<ITracingStructure> makeTracingStructure(VulkanDevice &device) {
    return std::make_unique<VulkanTracingStructure>(device);
}

VulkanTracingStructure &toVulkanTracingStructure(ITracingStructure &structure) {
    auto *result = dynamic_cast<VulkanTracingStructure *>(&structure);
    if (!result) {
        throw std::invalid_argument("Tracing structure is not implemented by Vulkan");
    }
    return *result;
}

TracingStructure VulkanTracingStructure::handle() const {
    return detail::HandleAccess::make<TracingStructureTag>(reinterpret_cast<uintptr_t>(_tlas));
}

void VulkanTracingStructure::deinit() {
    _raygenSbt.reset();
    _raygenPipeline = VK_NULL_HANDLE;
    _raygenSbtRegion = {};
    if (_blas) {
        vkDestroyAccelerationStructureKHR(_device.handle(), _blas, nullptr);
        _blas = VK_NULL_HANDLE;
    }
    _blasAddress = 0;
    if (_tlas) {
        vkDestroyAccelerationStructureKHR(_device.handle(), _tlas, nullptr);
        _tlas = VK_NULL_HANDLE;
    }
    for (auto &card : _cardTemplates) {
        if (card.structure) {
            vkDestroyAccelerationStructureKHR(_device.handle(), card.structure, nullptr);
            card.structure = VK_NULL_HANDLE;
        }
        card.storage.reset();
        card.address = 0;
        card.capacity = 0;
    }
    _blasStorage.reset();
    _tlasStorage.reset();
    _scratch.reset();
    _blasStorageCapacity = 0;
    _tlasStorageCapacity = 0;
    _scratchCapacity = 0;
    _instanceCapacity = 0;
    _cardGeneration = 0;
    _preparedInstanceCount = 0;
    _addressedInstanceBuffer = VK_NULL_HANDLE;
    _instanceAddresses = {};
    _addressedInstanceGeneration = 0;
    _rebuildCards = false;
}

void VulkanTracingStructure::traceRays(VkCommandBuffer commandBuffer, VkPipeline pipeline,
                                       glm::uvec2 extent) {
    if (_raygenPipeline != pipeline) {
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 deviceProperties {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        deviceProperties.pNext = &properties;
        vkGetPhysicalDeviceProperties2(_device.physicalDevice(), &deviceProperties);
        const auto alignUp = [](VkDeviceSize value, VkDeviceSize alignment) {
            return (value + alignment - 1) & ~(alignment - 1);
        };
        const VkDeviceSize recordSize = alignUp(properties.shaderGroupHandleSize,
                                                properties.shaderGroupHandleAlignment);
        const VkDeviceSize allocationSize =
            recordSize + properties.shaderGroupBaseAlignment - 1;
        std::vector<uint8_t> handle(properties.shaderGroupHandleSize);
        if (vkGetRayTracingShaderGroupHandlesKHR(_device.handle(), pipeline, 0, 1,
                                                 handle.size(), handle.data()) != VK_SUCCESS) {
            throw std::runtime_error("Vulkan: raygen shader-group handle query failed");
        }
        _raygenSbt = std::make_unique<VulkanBuffer>(_device);
        _raygenSbt->initHostVisible(
            allocationSize, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
        const VkDeviceAddress address = alignUp(_raygenSbt->deviceAddress(),
                                                properties.shaderGroupBaseAlignment);
        const VkDeviceSize offset = address - _raygenSbt->deviceAddress();
        std::memcpy(static_cast<uint8_t *>(_raygenSbt->mapped()) + offset, handle.data(),
                    handle.size());
        _raygenSbtRegion = {address, recordSize, recordSize};
        _raygenPipeline = pipeline;
    }
    const VkStridedDeviceAddressRegionKHR emptySbt {};
    vkCmdTraceRaysKHR(commandBuffer, &_raygenSbtRegion, &emptySbt, &emptySbt, &emptySbt,
                      extent.x, extent.y, 1);
}

void VulkanTracingStructure::prepare(const SceneTracingGeometry &geometry) {
    if (!geometry.vertices.buffer || !geometry.indices.buffer || !geometry.instances ||
        geometry.vertexCount == 0 || geometry.triangleCount == 0) {
        throw std::invalid_argument("Vulkan: incomplete scene tracing geometry");
    }
    const uint64_t expectedInstanceCount =
        1 + static_cast<uint64_t>(kGrassCardVariants) * geometry.cardRegionCapacity;
    if (geometry.instanceCount != expectedInstanceCount) {
        throw std::invalid_argument("Vulkan: invalid tracing instance region layout");
    }
    const VkDeviceAddress geometryAddress =
        toVulkanBuffer(*geometry.vertices.buffer).deviceAddress() + geometry.vertices.offset;
    std::array<VkAccelerationStructureGeometryTrianglesDataKHR, 2> triangleData {};
    std::array<VkAccelerationStructureGeometryKHR, 2> blasGeometries {};
    for (auto &triangles : triangleData) {
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = geometryAddress;
        triangles.vertexStride = sizeof(MergedVertex);
        triangles.maxVertex = geometry.vertexCount - 1;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress =
            toVulkanBuffer(*geometry.indices.buffer).deviceAddress() + geometry.indices.offset;
    }
    for (uint32_t i = 0; i < blasGeometries.size(); ++i) {
        auto &blasGeometry = blasGeometries[i];
        blasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        blasGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        blasGeometry.geometry.triangles = triangleData[i];
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
    // Geometry 1 stops short of the procedural sprites. They are a suffix of
    // the non-opaque range, so every primitive the structure still holds keeps
    // the index a candidate resolves through.
    const std::array<uint32_t, 2> blasPrimitiveCounts {{
        geometry.opaqueTriangleCount,
        geometry.triangleCount - geometry.opaqueTriangleCount -
            geometry.spriteTriangleCount,
    }};
    VkAccelerationStructureBuildSizesInfoKHR blasSizes {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(_device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &blasBuild, blasPrimitiveCounts.data(), &blasSizes);
    if (!_blas || blasSizes.accelerationStructureSize > _blasStorageCapacity) {
        if (_blas) {
            vkDestroyAccelerationStructureKHR(_device.handle(), _blas, nullptr);
            _blas = VK_NULL_HANDLE;
        }
        _blasStorage.reset();
        _blasStorageCapacity = grownCapacity(_blasStorageCapacity,
                                             blasSizes.accelerationStructureSize, 64 * 1024);
        _blasStorage = std::make_unique<VulkanBuffer>(_device);
        _blasStorage->initDeviceLocal(_blasStorageCapacity,
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      nullptr);
        VkAccelerationStructureCreateInfoKHR create {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer = _blasStorage->handle();
        create.size = _blasStorageCapacity;
        create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(_device.handle(), &create, nullptr, &_blas) != VK_SUCCESS) {
            throw std::runtime_error("Vulkan: merged BLAS creation failed");
        }
        _blasAddress = accelerationStructureAddress(_device, _blas);
    }

    const bool hasCards = geometry.cardRegionCapacity != 0;
    if (hasCards && (!geometry.cardVertices.buffer || !geometry.cardIndices.buffer ||
                     geometry.cardVertexCount == 0 || geometry.cardTriangleCount == 0)) {
        throw std::invalid_argument("Vulkan: incomplete grass-card tracing geometry");
    }
    _rebuildCards = hasCards &&
        (_cardGeneration != geometry.cardGeneration || _cardTemplates[0].structure == VK_NULL_HANDLE);
    std::array<VkAccelerationStructureBuildSizesInfoKHR, kGrassCardVariants> cardSizes {};
    if (hasCards) {
        for (uint32_t variant = 0; variant < kGrassCardVariants; ++variant) {
            VkAccelerationStructureGeometryKHR card = cardGeometry(geometry, variant);
            VkAccelerationStructureBuildGeometryInfoKHR cardBuild {
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            cardBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            cardBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            cardBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            cardBuild.geometryCount = 1;
            cardBuild.pGeometries = &card;
            const uint32_t primitiveCount = geometry.cardTriangleCount;
            cardSizes[variant].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            vkGetAccelerationStructureBuildSizesKHR(
                _device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &cardBuild,
                &primitiveCount, &cardSizes[variant]);
            auto &templateBlas = _cardTemplates[variant];
            if (!templateBlas.structure ||
                cardSizes[variant].accelerationStructureSize > templateBlas.capacity) {
                if (templateBlas.structure) {
                    vkDestroyAccelerationStructureKHR(_device.handle(), templateBlas.structure, nullptr);
                    templateBlas.structure = VK_NULL_HANDLE;
                    templateBlas.address = 0;
                }
                templateBlas.storage.reset();
                templateBlas.capacity = grownCapacity(templateBlas.capacity,
                                                       cardSizes[variant].accelerationStructureSize,
                                                       64 * 1024);
                templateBlas.storage = std::make_unique<VulkanBuffer>(_device);
                templateBlas.storage->initDeviceLocal(
                    templateBlas.capacity,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    nullptr);
                VkAccelerationStructureCreateInfoKHR create {
                    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
                create.buffer = templateBlas.storage->handle();
                create.size = templateBlas.capacity;
                create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                if (vkCreateAccelerationStructureKHR(_device.handle(), &create, nullptr,
                                                     &templateBlas.structure) != VK_SUCCESS) {
                    throw std::runtime_error("Vulkan: grass-card BLAS creation failed");
                }
                templateBlas.address =
                    accelerationStructureAddress(_device, templateBlas.structure);
                _rebuildCards = true;
            }
        }
    }

    VkAccelerationStructureGeometryInstancesDataKHR instanceData {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instanceData.arrayOfPointers = VK_FALSE;
    auto &instanceBuffer = toVulkanBuffer(*geometry.instances);
    instanceData.data.deviceAddress = instanceBuffer.deviceAddress();
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
    const uint32_t tlasInstanceCount = geometry.instanceCount;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(_device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &tlasBuild, &tlasInstanceCount, &tlasSizes);
    if (!_tlas || tlasSizes.accelerationStructureSize > _tlasStorageCapacity) {
        if (_tlas) {
            vkDestroyAccelerationStructureKHR(_device.handle(), _tlas, nullptr);
            _tlas = VK_NULL_HANDLE;
        }
        _tlasStorage.reset();
        _tlasStorageCapacity = grownCapacity(_tlasStorageCapacity,
                                             tlasSizes.accelerationStructureSize, 64 * 1024);
        _tlasStorage = std::make_unique<VulkanBuffer>(_device);
        _tlasStorage->initDeviceLocal(_tlasStorageCapacity,
                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      nullptr);
        VkAccelerationStructureCreateInfoKHR create {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer = _tlasStorage->handle();
        create.size = _tlasStorageCapacity;
        create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (vkCreateAccelerationStructureKHR(_device.handle(), &create, nullptr, &_tlas) != VK_SUCCESS) {
            throw std::runtime_error("Vulkan: TLAS creation failed");
        }
    }

    _instanceCapacity = tlasInstanceCount;
    _preparedInstanceCount = geometry.instanceCount;
    std::array<VkDeviceAddress, 1 + kGrassCardVariants> addresses {};
    addresses[0] = _blasAddress;
    if (hasCards) {
        for (uint32_t variant = 0; variant < kGrassCardVariants; ++variant) {
            addresses[1 + variant] = _cardTemplates[variant].address;
        }
    }
    if (_addressedInstanceBuffer != instanceBuffer.handle() ||
        _addressedInstanceGeneration != geometry.instanceGeneration ||
        _instanceAddresses != addresses) {
        std::vector<VkDeviceAddress> references(geometry.instanceCount, addresses[0]);
        for (uint32_t variant = 0; variant < kGrassCardVariants; ++variant) {
            const auto first = references.begin() + 1 + variant * geometry.cardRegionCapacity;
            std::fill_n(first, geometry.cardRegionCapacity, addresses[1 + variant]);
        }
        static_assert(sizeof(VkAccelerationStructureInstanceKHR) == 64);
        static_assert(offsetof(VkAccelerationStructureInstanceKHR,
                               accelerationStructureReference) == 56);
        instanceBuffer.uploadDeviceLocalStrided(
            offsetof(VkAccelerationStructureInstanceKHR, accelerationStructureReference),
            sizeof(VkAccelerationStructureInstanceKHR), sizeof(VkDeviceAddress),
            geometry.instanceCount, references.data());
        _addressedInstanceBuffer = instanceBuffer.handle();
        _instanceAddresses = addresses;
        _addressedInstanceGeneration = geometry.instanceGeneration;
    }
}

void VulkanTracingStructure::build(VkCommandBuffer commandBuffer,
                                   const SceneTracingGeometry &geometry) {
    if (_preparedInstanceCount != geometry.instanceCount ||
        _instanceCapacity != geometry.instanceCount) {
        throw std::logic_error("Vulkan: tracing structure was not prepared for this scene");
    }
    const VkDeviceAddress geometryAddress =
        toVulkanBuffer(*geometry.vertices.buffer).deviceAddress() + geometry.vertices.offset;
    std::array<VkAccelerationStructureGeometryTrianglesDataKHR, 2> triangleData {};
    std::array<VkAccelerationStructureGeometryKHR, 2> blasGeometries {};
    for (auto &triangles : triangleData) {
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = geometryAddress;
        triangles.vertexStride = sizeof(MergedVertex);
        triangles.maxVertex = geometry.vertexCount - 1;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress =
            toVulkanBuffer(*geometry.indices.buffer).deviceAddress() + geometry.indices.offset;
    }
    for (uint32_t i = 0; i < blasGeometries.size(); ++i) {
        blasGeometries[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        blasGeometries[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        blasGeometries[i].geometry.triangles = triangleData[i];
    }
    blasGeometries[0].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    blasGeometries[1].flags = 0;
    const std::array<uint32_t, 2> blasPrimitiveCounts {{
        geometry.opaqueTriangleCount,
        geometry.triangleCount - geometry.opaqueTriangleCount -
            geometry.spriteTriangleCount}};
    VkAccelerationStructureBuildGeometryInfoKHR mergedBuild {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    mergedBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    mergedBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    mergedBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    mergedBuild.geometryCount = static_cast<uint32_t>(blasGeometries.size());
    mergedBuild.pGeometries = blasGeometries.data();
    VkAccelerationStructureBuildSizesInfoKHR mergedSizes {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(_device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &mergedBuild, blasPrimitiveCounts.data(), &mergedSizes);

    std::array<VkAccelerationStructureGeometryKHR, kGrassCardVariants> cardGeometries {};
    std::array<VkAccelerationStructureBuildGeometryInfoKHR, kGrassCardVariants> cardBuilds {};
    std::array<VkAccelerationStructureBuildSizesInfoKHR, kGrassCardVariants> cardSizes {};
    const bool rebuildCards = _rebuildCards;
    if (rebuildCards) {
        for (uint32_t variant = 0; variant < kGrassCardVariants; ++variant) {
            cardGeometries[variant] = cardGeometry(geometry, variant);
            auto &build = cardBuilds[variant];
            build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            build.geometryCount = 1;
            build.pGeometries = &cardGeometries[variant];
            const uint32_t count = geometry.cardTriangleCount;
            cardSizes[variant].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            vkGetAccelerationStructureBuildSizesKHR(_device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                    &build, &count, &cardSizes[variant]);
        }
    }

    VkAccelerationStructureGeometryInstancesDataKHR instanceData {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instanceData.arrayOfPointers = VK_FALSE;
    instanceData.data.deviceAddress = toVulkanBuffer(*geometry.instances).deviceAddress();
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
    const uint32_t tlasInstanceCount = geometry.instanceCount;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(_device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &tlasBuild, &tlasInstanceCount, &tlasSizes);

    const auto alignment = _device.accelerationStructureProperties().minAccelerationStructureScratchOffsetAlignment;
    std::array<VkDeviceSize, 1 + kGrassCardVariants + 1> scratchOffsets {};
    VkDeviceSize scratchSize = 0;
    const auto reserveScratch = [&](VkDeviceSize size, uint32_t index) {
        if (scratchSize > std::numeric_limits<VkDeviceSize>::max() - (alignment - 1)) {
            throw std::runtime_error("Vulkan: acceleration-structure scratch size exceeds device-size range");
        }
        const VkDeviceSize mask = alignment - 1;
        scratchSize = (scratchSize + mask) & ~mask;
        scratchOffsets[index] = scratchSize;
        if (size > std::numeric_limits<VkDeviceSize>::max() - scratchSize) {
            throw std::runtime_error("Vulkan: acceleration-structure scratch size exceeds device-size range");
        }
        scratchSize += size;
    };
    reserveScratch(mergedSizes.buildScratchSize, 0);
    if (rebuildCards) {
        for (uint32_t variant = 0; variant < kGrassCardVariants; ++variant)
            reserveScratch(cardSizes[variant].buildScratchSize, 1 + variant);
    }
    reserveScratch(tlasSizes.buildScratchSize, 1 + kGrassCardVariants);
    if (scratchSize > std::numeric_limits<VkDeviceSize>::max() - alignment) {
        throw std::runtime_error("Vulkan: acceleration-structure scratch size exceeds device-size range");
    }
    const VkDeviceSize scratchAllocationSize = scratchSize + alignment;
    if (!_scratch || scratchAllocationSize > _scratchCapacity) {
        _scratch.reset();
        _scratchCapacity = grownCapacity(_scratchCapacity, scratchAllocationSize, 64 * 1024);
        _scratch = std::make_unique<VulkanBuffer>(_device);
        _scratch->initDeviceLocal(_scratchCapacity,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  nullptr);
    }
    const VkDeviceAddress scratchAddress = alignedAddress(_scratch->deviceAddress(), alignment);
    mergedBuild.dstAccelerationStructure = _blas;
    mergedBuild.scratchData.deviceAddress = scratchAddress + scratchOffsets[0];
    std::array<VkAccelerationStructureBuildRangeInfoKHR, 2> blasRanges {};
    blasRanges[0].primitiveCount = blasPrimitiveCounts[0];
    blasRanges[1].primitiveCount = blasPrimitiveCounts[1];
    blasRanges[1].primitiveOffset =
        static_cast<uint32_t>(geometry.opaqueTriangleCount * 3 * sizeof(uint32_t));
    std::array<VkAccelerationStructureBuildGeometryInfoKHR, 1 + kGrassCardVariants> bottomBuilds {};
    std::array<std::array<VkAccelerationStructureBuildRangeInfoKHR, 2>, 1 + kGrassCardVariants> bottomRanges {};
    std::array<const VkAccelerationStructureBuildRangeInfoKHR *, 1 + kGrassCardVariants> bottomRangePointers {};
    bottomBuilds[0] = mergedBuild;
    bottomRanges[0][0] = blasRanges[0];
    bottomRanges[0][1] = blasRanges[1];
    bottomRangePointers[0] = bottomRanges[0].data();
    uint32_t bottomCount = 1;
    if (rebuildCards) {
        for (uint32_t variant = 0; variant < kGrassCardVariants; ++variant) {
            auto build = cardBuilds[variant];
            build.dstAccelerationStructure = _cardTemplates[variant].structure;
            build.scratchData.deviceAddress = scratchAddress + scratchOffsets[1 + variant];
            bottomBuilds[bottomCount] = build;
            bottomRanges[bottomCount][0].primitiveCount = geometry.cardTriangleCount;
            bottomRangePointers[bottomCount] = bottomRanges[bottomCount].data();
            ++bottomCount;
        }
    }
    vkCmdBuildAccelerationStructuresKHR(commandBuffer, bottomCount, bottomBuilds.data(),
                                        bottomRangePointers.data());

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
    vkCmdPipelineBarrier2(commandBuffer, &blasToTlasDependency);

    tlasBuild.dstAccelerationStructure = _tlas;
    tlasBuild.scratchData.deviceAddress = scratchAddress + scratchOffsets[1 + kGrassCardVariants];
    VkAccelerationStructureBuildRangeInfoKHR tlasRange {};
    tlasRange.primitiveCount = tlasInstanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR *tlasRanges[] {&tlasRange};
    vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &tlasBuild, tlasRanges);

    VkMemoryBarrier2 tlasToTrace {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    tlasToTrace.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    tlasToTrace.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    tlasToTrace.dstStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    tlasToTrace.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo tlasToTraceDependency {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    tlasToTraceDependency.memoryBarrierCount = 1;
    tlasToTraceDependency.pMemoryBarriers = &tlasToTrace;
    vkCmdPipelineBarrier2(commandBuffer, &tlasToTraceDependency);
    if (rebuildCards) {
        _cardGeneration = geometry.cardGeneration;
        _rebuildCards = false;
    }
}

} // namespace reone::graphics
