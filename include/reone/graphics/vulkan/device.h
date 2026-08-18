/*
 * Copyright (c) 2020-2026 The reone project contributors
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

#include <array>

#include <volk.h>

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

struct SDL_Window;

namespace reone {

namespace graphics {

struct VulkanRayQueryFeatures {
    uint32_t apiVersion {VK_API_VERSION_1_3};
    VkPhysicalDeviceFeatures core {};
    VkPhysicalDeviceVulkan12Features vulkan12 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipeline {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    std::array<const char *, 4> extensions {{
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    }};
};

struct VulkanFsrFeatures {
    VkPhysicalDeviceFeatures core {};
    VkPhysicalDeviceVulkan12Features vulkan12 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    bool available {true};
};

/**
 * The Vulkan instance, the physical device we chose, the logical device and the
 * queues - everything that lives for the whole run and that everything else
 * needs a handle to.
 *
 * Ownership is deliberately coarse. These objects are created once, destroyed
 * last, and are never recreated, unlike the swapchain.
 */
class VulkanDevice : boost::noncopyable {
public:
    ~VulkanDevice() { deinit(); }

    /** The requirements shared by ray-query selection and logical-device creation. */
    static VulkanRayQueryFeatures rayQueryFeatures(
        const VkPhysicalDeviceFeatures &rasterCore,
        const VkPhysicalDeviceVulkan12Features &rasterVulkan12);

    /** Features needed by the half-precision permutations FSR selects itself. */
    static VulkanFsrFeatures fsrFeatures(
        const VkPhysicalDeviceFeatures &supportedCore,
        const VkPhysicalDeviceVulkan12Features &supportedVulkan12);

    /** Set the core feature payload on the object handed to DeviceBuilder. */
    static vkb::PhysicalDevice prepareLogicalDevice(
        vkb::PhysicalDevice physicalDevice,
        const VkPhysicalDeviceFeatures &coreFeatures);

    /**
     * @param window an SDL window created with SDL_WINDOW_VULKAN; its surface is
     *               what the physical device is selected against, because a
     *               device that cannot present to it is no use to us.
     * @param validation request the validation layers and a debug messenger
     */
    void init(SDL_Window *window, bool validation);
    void deinit();

    VkInstance instance() const { return _instance.instance; }
    VkPhysicalDevice physicalDevice() const { return _device.physical_device; }
    VkDevice handle() const { return _device.device; }
    VkSurfaceKHR surface() const { return _surface; }

    /** Every device allocation goes through here; nothing calls vkAllocateMemory. */
    VmaAllocator allocator() const { return _allocator; }

    VkQueue graphicsQueue() const { return _graphicsQueue; }
    uint32_t graphicsQueueFamily() const { return _graphicsQueueFamily; }

    /**
     * Record and run @p block on the graphics queue, blocking until it has
     * finished. For load-time work - staging copies, layout transitions of
     * freshly created images - where the simplicity is worth the stall.
     */
    void immediateSubmit(const std::function<void(VkCommandBuffer)> &block);

    /**
     * Block until the device has finished everything. Required before
     * destroying anything the GPU might still be reading, which in practice
     * means before any teardown that is not the renderer's own.
     */
    void waitIdle() const { vkDeviceWaitIdle(_device.device); }

    /**
     * Name a Vulkan object, so a capture shows "g-buffer diffuse" rather than
     * VkImage 0x43. Costs nothing when the debug utils extension is absent.
     */
    void setObjectName(VkObjectType type, uint64_t handle, const std::string &name) const;

    /**
     * Open and close a labelled region in a command buffer, which is what a
     * graphics debugger groups its event list by.
     *
     * Prefer VulkanDebugScope over calling these directly - it cannot leave a
     * region open on an early return.
     */
    void beginLabel(VkCommandBuffer cmd, const char *name, const glm::vec3 &color) const;
    void endLabel(VkCommandBuffer cmd) const;

    /** Whether labels and names actually reach anything. */
    bool debugUtilsAvailable() const { return _debugUtils; }

    /**
     * Whether this device was created with the optional ray-query acceleration
     * structure capability. Raster rendering does not depend on it.
     */
    bool rayQueryAvailable() const { return _rayQueryAvailable; }

    bool fsrAvailable() const { return _fsrAvailable; }

    /** Limits that every later acceleration-structure build must observe. */
    const VkPhysicalDeviceAccelerationStructurePropertiesKHR &
    accelerationStructureProperties() const {
        return _accelerationStructureProperties;
    }

    /** Maximum sampled images usable by the ray-query bindless descriptor set. */
    uint32_t maxBindlessSampledImages() const { return _maxBindlessSampledImages; }

    /** The largest anisotropy this device will accept in a sampler. */
    float maxAnisotropy() const { return _maxAnisotropy; }

    /** Round @p size up to the minimum uniform buffer offset alignment. */
    VkDeviceSize alignUniform(VkDeviceSize size) const;

    const std::string &deviceName() const { return _deviceName; }

    /** Access for the pieces that need to build on top of it. */
    vkb::Device &bootstrapDevice() { return _device; }

private:
    bool _inited {false};

    vkb::Instance _instance;
    vkb::Device _device;
    VkSurfaceKHR _surface {VK_NULL_HANDLE};
    VmaAllocator _allocator {VK_NULL_HANDLE};

    bool _debugUtils {false};
    bool _rayQueryAvailable {false};
    bool _fsrAvailable {false};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR _accelerationStructureProperties {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    uint32_t _maxBindlessSampledImages {0};

    VkQueue _graphicsQueue {VK_NULL_HANDLE};
    uint32_t _graphicsQueueFamily {0};

    std::string _deviceName;
    VkDeviceSize _uniformAlignment {256};
    float _maxAnisotropy {1.0f};

    VkCommandPool _immediatePool {VK_NULL_HANDLE};
    VkCommandBuffer _immediateBuffer {VK_NULL_HANDLE};
    VkFence _immediateFence {VK_NULL_HANDLE};
};

} // namespace graphics

} // namespace reone
