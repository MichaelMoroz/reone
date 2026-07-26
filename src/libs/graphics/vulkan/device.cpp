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

#include "reone/graphics/vulkan/device.h"

#include <SDL3/SDL_vulkan.h>

#include "reone/system/logutil.h"

namespace reone {

namespace graphics {

void VulkanDevice::init(SDL_Window *window, bool validation) {
    if (_inited) {
        return;
    }
    // volk must load before any Vulkan call: nothing links vulkan-1, so every
    // entry point including vkCreateInstance starts out null.
    if (volkInitialize() != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: no loader found");
    }

    auto instanceResult = vkb::InstanceBuilder()
                              .set_app_name("reone")
                              .require_api_version(1, 3, 0)
                              .request_validation_layers(validation)
                              .use_default_debug_messenger()
                              .build();
    if (!instanceResult) {
        throw std::runtime_error("Vulkan: instance creation failed: " +
                                 instanceResult.error().message());
    }
    _instance = instanceResult.value();
    volkLoadInstance(_instance.instance);

    if (!SDL_Vulkan_CreateSurface(window, _instance.instance, nullptr, &_surface)) {
        throw std::runtime_error("Vulkan: surface creation failed: " +
                                 std::string(SDL_GetError()));
    }

    // Selecting against the surface rules out any device that cannot present to
    // this window, which on a laptop with switchable graphics is a real case.
    VkPhysicalDeviceVulkan13Features features13 {};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    auto physicalResult = vkb::PhysicalDeviceSelector(_instance)
                              .set_surface(_surface)
                              .set_minimum_version(1, 3)
                              .set_required_features_13(features13)
                              .select();
    if (!physicalResult) {
        throw std::runtime_error("Vulkan: no suitable device: " +
                                 physicalResult.error().message());
    }

    auto deviceResult = vkb::DeviceBuilder(physicalResult.value()).build();
    if (!deviceResult) {
        throw std::runtime_error("Vulkan: logical device creation failed: " +
                                 deviceResult.error().message());
    }
    _device = deviceResult.value();
    volkLoadDevice(_device.device);

    auto queueResult = _device.get_queue(vkb::QueueType::graphics);
    if (!queueResult) {
        throw std::runtime_error("Vulkan: no graphics queue: " +
                                 queueResult.error().message());
    }
    _graphicsQueue = queueResult.value();
    _graphicsQueueFamily = _device.get_queue_index(vkb::QueueType::graphics).value();

    // VMA resolves its own entry points, but volk owns them here, so they have
    // to be handed over explicitly or it calls through null pointers.
    VmaVulkanFunctions vmaFunctions {};
    vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo {};
    allocatorInfo.physicalDevice = _device.physical_device;
    allocatorInfo.device = _device.device;
    allocatorInfo.instance = _instance.instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorInfo.pVulkanFunctions = &vmaFunctions;
    if (vmaCreateAllocator(&allocatorInfo, &_allocator) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: allocator creation failed");
    }

    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(_device.physical_device, &props);
    _deviceName = props.deviceName;
    info("Vulkan device: " + _deviceName);

    _inited = true;
}

void VulkanDevice::deinit() {
    if (!_inited) {
        return;
    }
    if (_allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(_allocator);
        _allocator = VK_NULL_HANDLE;
    }
    vkb::destroy_device(_device);
    if (_surface != VK_NULL_HANDLE) {
        vkb::destroy_surface(_instance, _surface);
        _surface = VK_NULL_HANDLE;
    }
    vkb::destroy_instance(_instance);
    _inited = false;
}

} // namespace graphics

} // namespace reone
