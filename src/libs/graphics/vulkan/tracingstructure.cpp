/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/graphics/vulkan/tracingstructure.h"

#include "reone/graphics/gpuscene.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/rhi.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace reone::graphics {
namespace {

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
    _instances.reset();
    _raygenSbt.reset();
    _raygenPipeline = VK_NULL_HANDLE;
    _raygenSbtRegion = {};
    if (_blas) {
        vkDestroyAccelerationStructureKHR(_device.handle(), _blas, nullptr);
        _blas = VK_NULL_HANDLE;
    }
    if (_tlas) {
        vkDestroyAccelerationStructureKHR(_device.handle(), _tlas, nullptr);
        _tlas = VK_NULL_HANDLE;
    }
    _blasStorage.reset();
    _tlasStorage.reset();
    _scratch.reset();
    _blasStorageCapacity = 0;
    _tlasStorageCapacity = 0;
    _scratchCapacity = 0;
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

void VulkanTracingStructure::build(VkCommandBuffer commandBuffer,
                                   const SceneTracingGeometry &geometry) {
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
    const std::array<uint32_t, 2> blasPrimitiveCounts {{
        geometry.opaqueTriangleCount,
        geometry.triangleCount - geometry.opaqueTriangleCount,
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
    }

    VkAccelerationStructureDeviceAddressInfoKHR blasAddressInfo {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    blasAddressInfo.accelerationStructure = _blas;
    VkAccelerationStructureInstanceKHR instance {};
    instance.transform = instanceTransform(glm::mat4(1.0f));
    instance.instanceCustomIndex = 0;
    instance.mask = 0xff;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference =
        vkGetAccelerationStructureDeviceAddressKHR(_device.handle(), &blasAddressInfo);
    _instances = std::make_unique<VulkanBuffer>(_device);
    _instances->initHostVisible(sizeof(instance), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
    std::memcpy(_instances->mapped(), &instance, sizeof(instance));

    VkAccelerationStructureGeometryInstancesDataKHR instanceData {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    instanceData.arrayOfPointers = VK_FALSE;
    instanceData.data.deviceAddress = _instances->deviceAddress();
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
    vkGetAccelerationStructureBuildSizesKHR(_device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &tlasBuild, &kTlasInstanceCount, &tlasSizes);
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

    const auto alignment = _device.accelerationStructureProperties().minAccelerationStructureScratchOffsetAlignment;
    const VkDeviceSize scratchSize = std::max(blasSizes.buildScratchSize, tlasSizes.buildScratchSize);
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
    blasBuild.dstAccelerationStructure = _blas;
    blasBuild.scratchData.deviceAddress = scratchAddress;
    std::array<VkAccelerationStructureBuildRangeInfoKHR, 2> blasRanges {};
    blasRanges[0].primitiveCount = blasPrimitiveCounts[0];
    blasRanges[1].primitiveCount = blasPrimitiveCounts[1];
    blasRanges[1].primitiveOffset =
        static_cast<uint32_t>(geometry.opaqueTriangleCount * 3 * sizeof(uint32_t));
    const VkAccelerationStructureBuildRangeInfoKHR *blasRangePointers[] {
        &blasRanges[0], &blasRanges[1]};
    vkCmdBuildAccelerationStructuresKHR(commandBuffer, 1, &blasBuild, blasRangePointers);

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
    tlasBuild.scratchData.deviceAddress = scratchAddress;
    VkAccelerationStructureBuildRangeInfoKHR tlasRange {};
    tlasRange.primitiveCount = kTlasInstanceCount;
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
}

} // namespace reone::graphics
