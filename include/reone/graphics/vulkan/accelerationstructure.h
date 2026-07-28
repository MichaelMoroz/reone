/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <volk.h>

#include "buffer.h"

namespace reone::graphics {

class VulkanDevice;
class VulkanMesh;

/** A static bottom-level acceleration structure owned with its mesh upload. */
class VulkanBLAS : boost::noncopyable {
public:
    VulkanBLAS(VulkanDevice &device, const VulkanMesh &mesh);
    ~VulkanBLAS() { deinit(); }

    void init();
    void deinit();
    VkAccelerationStructureKHR handle() const { return _handle; }
    uint64_t buildMicroseconds() const { return _buildMicroseconds; }

private:
    VulkanDevice &_device;
    const VulkanMesh &_mesh;
    std::unique_ptr<VulkanBuffer> _storage;
    VkAccelerationStructureKHR _handle {VK_NULL_HANDLE};
    uint64_t _buildMicroseconds {0};
};

} // namespace reone::graphics
