/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/graphics/vulkan/accelerationstructure.h"

#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/mesh.h"

#include <chrono>

namespace reone::graphics {

static VkDeviceAddress alignedAddress(VkDeviceAddress address, VkDeviceSize alignment) {
    return (address + alignment - 1) & ~(alignment - 1);
}

VulkanBLAS::VulkanBLAS(VulkanDevice &device, const VulkanMesh &mesh) :
    _device(device), _mesh(mesh) {}

void VulkanBLAS::init() {
    const auto geometry = _mesh.geometry();
    if (!geometry.vertexAddress || !geometry.indexAddress) {
        throw std::runtime_error("Vulkan: BLAS mesh has no device addresses");
    }

    VkAccelerationStructureGeometryTrianglesDataKHR triangles {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    triangles.vertexFormat = geometry.vertexFormat;
    triangles.vertexData.deviceAddress = geometry.vertexAddress;
    triangles.vertexStride = geometry.vertexStride;
    triangles.maxVertex = geometry.maxVertexIndex;
    triangles.indexType = geometry.indexType;
    triangles.indexData.deviceAddress = geometry.indexAddress;

    VkAccelerationStructureGeometryKHR asGeometry {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    asGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    asGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    asGeometry.geometry.triangles = triangles;

    const uint32_t primitiveCount = _mesh.indexCount() / 3;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &asGeometry;

    VkAccelerationStructureBuildSizesInfoKHR sizes {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(
        _device.handle(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
        &primitiveCount, &sizes);

    _storage = std::make_unique<VulkanBuffer>(_device);
    _storage->initDeviceLocal(sizes.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        nullptr);
    VkAccelerationStructureCreateInfoKHR createInfo {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    createInfo.buffer = _storage->handle();
    createInfo.size = sizes.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vkCreateAccelerationStructureKHR(_device.handle(), &createInfo, nullptr, &_handle) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: BLAS creation failed");
    }

    const auto alignment = _device.accelerationStructureProperties().minAccelerationStructureScratchOffsetAlignment;
    VulkanBuffer scratch(_device);
    scratch.initDeviceLocal(sizes.buildScratchSize + alignment,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr);
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.dstAccelerationStructure = _handle;
    buildInfo.scratchData.deviceAddress = alignedAddress(scratch.deviceAddress(), alignment);
    VkAccelerationStructureBuildRangeInfoKHR range {};
    range.primitiveCount = primitiveCount;
    const VkAccelerationStructureBuildRangeInfoKHR *ranges[] {&range};

    VkQueryPool queryPool {VK_NULL_HANDLE};
    VkQueryPoolCreateInfo queryInfo {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    queryInfo.queryCount = 1;
    if (vkCreateQueryPool(_device.handle(), &queryInfo, nullptr, &queryPool) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: BLAS compaction query pool creation failed");
    }
    const auto start = std::chrono::steady_clock::now();
    _device.immediateSubmit([&buildInfo, ranges, queryPool, this](VkCommandBuffer cmd) {
        vkCmdResetQueryPool(cmd, queryPool, 0, 1);
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, ranges);
        vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &_handle,
            VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, queryPool, 0);
        VkMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
    });
    uint64_t compactedSize = 0;
    if (vkGetQueryPoolResults(_device.handle(), queryPool, 0, 1, sizeof(compactedSize),
                              &compactedSize, sizeof(compactedSize), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
        vkDestroyQueryPool(_device.handle(), queryPool, nullptr);
        throw std::runtime_error("Vulkan: BLAS compaction query failed");
    }
    vkDestroyQueryPool(_device.handle(), queryPool, nullptr);
    if (compactedSize > 0 && compactedSize < sizes.accelerationStructureSize) {
        auto compactStorage = std::make_unique<VulkanBuffer>(_device);
        compactStorage->initDeviceLocal(compactedSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, nullptr);
        VkAccelerationStructureKHR compactHandle {VK_NULL_HANDLE};
        createInfo.buffer = compactStorage->handle();
        createInfo.size = compactedSize;
        if (vkCreateAccelerationStructureKHR(_device.handle(), &createInfo, nullptr, &compactHandle) != VK_SUCCESS) {
            throw std::runtime_error("Vulkan: compacted BLAS creation failed");
        }
        const auto source = _handle;
        _device.immediateSubmit([source, compactHandle](VkCommandBuffer cmd) {
            VkCopyAccelerationStructureInfoKHR copy {VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
            copy.src = source;
            copy.dst = compactHandle;
            copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
            vkCmdCopyAccelerationStructureKHR(cmd, &copy);
        });
        vkDestroyAccelerationStructureKHR(_device.handle(), _handle, nullptr);
        _handle = compactHandle;
        _storage = std::move(compactStorage);
    }
    _buildMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
}

void VulkanBLAS::deinit() {
    if (_handle != VK_NULL_HANDLE) {
        vkDestroyAccelerationStructureKHR(_device.handle(), _handle, nullptr);
        _handle = VK_NULL_HANDLE;
    }
    _storage.reset();
}

} // namespace reone::graphics
