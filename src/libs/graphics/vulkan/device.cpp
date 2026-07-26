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

    // Debug utils gives pass labels and object names in a graphics debugger,
    // which is most of what makes a capture readable. Wanted whether or not the
    // validation layers are on, so asked for explicitly - but only if the
    // loader actually has it, since a missing instance extension is fatal.
    auto systemInfo = vkb::SystemInfo::get_system_info();
    bool debugUtils = systemInfo &&
                      systemInfo->is_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    vkb::InstanceBuilder instanceBuilder;
    instanceBuilder.set_app_name("reone")
        .require_api_version(1, 3, 0)
        .request_validation_layers(validation)
        .use_default_debug_messenger();
    if (debugUtils) {
        instanceBuilder.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    auto instanceResult = instanceBuilder.build();
    if (!instanceResult) {
        throw std::runtime_error("Vulkan: instance creation failed: " +
                                 instanceResult.error().message());
    }
    _instance = instanceResult.value();
    volkLoadInstance(_instance.instance);
    // volk leaves the entry points null when the extension is absent, so this
    // is the honest test rather than what was asked for.
    _debugUtils = debugUtils && vkSetDebugUtilsObjectNameEXT != nullptr;

    if (!SDL_Vulkan_CreateSurface(window, _instance.instance, nullptr, &_surface)) {
        throw std::runtime_error("Vulkan: surface creation failed: " +
                                 std::string(SDL_GetError()));
    }

    // Selecting against the surface rules out any device that cannot present to
    // this window, which on a laptop with switchable graphics is a real case.
    // Slang lowers SV_VertexID and SV_InstanceID to VertexIndex/InstanceIndex
    // adjusted by BaseVertex/BaseInstance, which needs the DrawParameters
    // capability. This is the same capability OpenGL's SPIR-V path silently
    // ignored, reading the builtins as zero and collapsing every grass instance
    // onto one (§9.5). Here it is simply a feature to ask for.
    VkPhysicalDeviceVulkan11Features features11 {};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.shaderDrawParameters = VK_TRUE;
    // Shadow maps render every cascade, or every cube face, from one draw. The
    // OpenGL pipeline uses a geometry shader to fan the triangle out across
    // layers; multiview does the same thing without one.
    features11.multiview = VK_TRUE;

    VkPhysicalDeviceVulkan13Features features13 {};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    auto physicalResult = vkb::PhysicalDeviceSelector(_instance)
                              .set_surface(_surface)
                              .set_minimum_version(1, 3)
                              .set_required_features_11(features11)
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
    _uniformAlignment = props.limits.minUniformBufferOffsetAlignment;
    info("Vulkan device: " + _deviceName);
    if (!_debugUtils) {
        info("Vulkan: debug utils unavailable; captures will be unlabelled",
             LogChannel::Graphics);
    }

    VkCommandPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = _graphicsQueueFamily;
    if (vkCreateCommandPool(_device.device, &poolInfo, nullptr, &_immediatePool) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: immediate command pool creation failed");
    }
    VkCommandBufferAllocateInfo cmdInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.commandPool = _immediatePool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(_device.device, &cmdInfo, &_immediateBuffer) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: immediate command buffer allocation failed");
    }
    VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(_device.device, &fenceInfo, nullptr, &_immediateFence) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: immediate fence creation failed");
    }

    _inited = true;
}

void VulkanDevice::setObjectName(VkObjectType type, uint64_t handle,
                                 const std::string &name) const {
    if (!_debugUtils) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT info {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name.c_str();
    vkSetDebugUtilsObjectNameEXT(_device.device, &info);
}

void VulkanDevice::beginLabel(VkCommandBuffer cmd, const char *name,
                              const glm::vec3 &color) const {
    if (!_debugUtils || vkCmdBeginDebugUtilsLabelEXT == nullptr) {
        return;
    }
    VkDebugUtilsLabelEXT label {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    label.color[0] = color.r;
    label.color[1] = color.g;
    label.color[2] = color.b;
    label.color[3] = 1.0f;
    vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
}

void VulkanDevice::endLabel(VkCommandBuffer cmd) const {
    if (!_debugUtils || vkCmdEndDebugUtilsLabelEXT == nullptr) {
        return;
    }
    vkCmdEndDebugUtilsLabelEXT(cmd);
}

void VulkanDevice::deinit() {
    if (!_inited) {
        return;
    }
    if (_immediateFence != VK_NULL_HANDLE) {
        vkDestroyFence(_device.device, _immediateFence, nullptr);
        _immediateFence = VK_NULL_HANDLE;
    }
    if (_immediatePool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(_device.device, _immediatePool, nullptr);
        _immediatePool = VK_NULL_HANDLE;
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

void VulkanDevice::immediateSubmit(const std::function<void(VkCommandBuffer)> &block) {
    vkResetFences(_device.device, 1, &_immediateFence);
    vkResetCommandBuffer(_immediateBuffer, 0);

    VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(_immediateBuffer, &beginInfo);
    block(_immediateBuffer);
    vkEndCommandBuffer(_immediateBuffer);

    VkCommandBufferSubmitInfo cmdInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = _immediateBuffer;

    VkSubmitInfo2 submit {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    vkQueueSubmit2(_graphicsQueue, 1, &submit, _immediateFence);
    vkWaitForFences(_device.device, 1, &_immediateFence, VK_TRUE, UINT64_MAX);
}

VkDeviceSize VulkanDevice::alignUniform(VkDeviceSize size) const {
    return (size + _uniformAlignment - 1) & ~(_uniformAlignment - 1);
}

} // namespace graphics

} // namespace reone
