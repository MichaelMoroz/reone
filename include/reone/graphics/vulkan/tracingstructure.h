/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <volk.h>

#include "reone/graphics/tracingstructure.h"
#include "reone/graphics/vulkan/buffer.h"

namespace reone::graphics {

class VulkanDevice;

/** Vulkan implementation of a frame-local scene-wide tracing structure. */
class VulkanTracingStructure : public ITracingStructure, boost::noncopyable {
public:
    explicit VulkanTracingStructure(VulkanDevice &device) :
        _device(device) {
    }
    ~VulkanTracingStructure() { deinit(); }

    TracingStructure handle() const override;
    void deinit() override;
    void build(VkCommandBuffer commandBuffer, const SceneTracingGeometry &geometry);

private:
    VulkanDevice &_device;
    std::unique_ptr<VulkanBuffer> _instances;
    std::unique_ptr<VulkanBuffer> _blasStorage;
    std::unique_ptr<VulkanBuffer> _tlasStorage;
    std::unique_ptr<VulkanBuffer> _scratch;
    VkAccelerationStructureKHR _blas {VK_NULL_HANDLE};
    VkAccelerationStructureKHR _tlas {VK_NULL_HANDLE};
    VkDeviceSize _blasStorageCapacity {0};
    VkDeviceSize _tlasStorageCapacity {0};
    VkDeviceSize _scratchCapacity {0};
};

VulkanTracingStructure &toVulkanTracingStructure(ITracingStructure &structure);

} // namespace reone::graphics
