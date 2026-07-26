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

#include <volk.h>

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

struct SDL_Window;

namespace reone {

namespace graphics {

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
