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
#pragma once

#include <volk.h>

#include "reone/graphics/rhi/tracingstructure.h"
#include "reone/graphics/vulkan/buffer.h"

namespace reone::graphics {

class VulkanDevice;

std::unique_ptr<ITracingStructure> makeTracingStructure(VulkanDevice &device);

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
    void traceRays(VkCommandBuffer commandBuffer, VkPipeline pipeline,
                   glm::uvec2 extent);

private:
    VulkanDevice &_device;
    std::unique_ptr<VulkanBuffer> _instances;
    std::unique_ptr<VulkanBuffer> _blasStorage;
    std::unique_ptr<VulkanBuffer> _tlasStorage;
    std::unique_ptr<VulkanBuffer> _scratch;
    std::unique_ptr<VulkanBuffer> _raygenSbt;
    VkAccelerationStructureKHR _blas {VK_NULL_HANDLE};
    VkAccelerationStructureKHR _tlas {VK_NULL_HANDLE};
    VkPipeline _raygenPipeline {VK_NULL_HANDLE};
    VkStridedDeviceAddressRegionKHR _raygenSbtRegion {};
    VkDeviceSize _blasStorageCapacity {0};
    VkDeviceSize _tlasStorageCapacity {0};
    VkDeviceSize _scratchCapacity {0};
};

VulkanTracingStructure &toVulkanTracingStructure(ITracingStructure &structure);

} // namespace reone::graphics
