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

#include <algorithm>
#include <cstring>
#include <vector>

#include <SDL3/SDL_vulkan.h>

#include "reone/system/logutil.h"

namespace reone {

namespace graphics {

VulkanRayQueryFeatures VulkanDevice::rayQueryFeatures(
    const VkPhysicalDeviceFeatures &rasterCore,
    const VkPhysicalDeviceVulkan12Features &rasterVulkan12) {
    VulkanRayQueryFeatures result;
    result.core = rasterCore;
    result.vulkan12 = rasterVulkan12;
    result.vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    result.vulkan12.bufferDeviceAddress = VK_TRUE;
    result.accelerationStructure.accelerationStructure = VK_TRUE;
    result.rayQuery.rayQuery = VK_TRUE;
    result.rayTracingPipeline.rayTracingPipeline = VK_TRUE;
    return result;
}

VulkanFsrFeatures VulkanDevice::fsrFeatures(
    const VkPhysicalDeviceFeatures &supportedCore,
    const VkPhysicalDeviceVulkan12Features &supportedVulkan12) {
    VulkanFsrFeatures result;
    const bool halfPrecision = supportedCore.shaderInt16 == VK_TRUE &&
                               supportedVulkan12.shaderFloat16 == VK_TRUE;
    result.core.shaderInt16 = halfPrecision ? VK_TRUE : VK_FALSE;
    result.vulkan12.shaderFloat16 = halfPrecision ? VK_TRUE : VK_FALSE;
    // FSR2 falls back to its FP32 permutations when Float16 is absent. When
    // Float16 exists without Int16, however, its Vulkan backend selects blobs
    // that declare both capabilities, so creating the upscaler would be invalid.
    result.available = supportedVulkan12.shaderFloat16 != VK_TRUE || halfPrecision;
    return result;
}

vkb::PhysicalDevice VulkanDevice::prepareLogicalDevice(
    vkb::PhysicalDevice physicalDevice,
    const VkPhysicalDeviceFeatures &coreFeatures) {
    // DeviceBuilder copies this member into VkPhysicalDeviceFeatures2::features
    // when it constructs VkDeviceCreateInfo. Selector requirements alone are
    // not the creation seam; make the payload at that seam explicit.
    physicalDevice.features = coreFeatures;
    return physicalDevice;
}

void VulkanDevice::init(SDL_Window *window, bool validation, bool debugLabels) {
    if (_inited) {
        return;
    }
#ifdef R_ENABLE_DLSS
    // Ahead of everything, because Streamline requires the VkDevice be created
    // after slInit, and because its feature requirements have to be answered
    // before the selectors below run. Failure is silent and ordinary: no DLLs
    // is the state every user starts in.
    //
    // Note this does NOT take over the loader. Manual hooking leaves volk
    // owning vulkan-1.dll; see StreamlineRuntime.
    _streamline.init();
#endif

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
#ifdef R_ENABLE_DLSS
    // Requested rather than required: a missing one must cost DLSS, not the
    // instance. Measured on an RTX 5090 these are three capability extensions
    // every 1.3 driver advertises, so the fallback is theoretical.
    for (const auto *extension : _streamline.instanceExtensions()) {
        instanceBuilder.enable_extension(extension);
    }
#endif
    auto instanceResult = instanceBuilder.build();
    if (!instanceResult) {
        throw std::runtime_error("Vulkan: instance creation failed: " +
                                 instanceResult.error().message());
    }
    _instance = instanceResult.value();
    volkLoadInstance(_instance.instance);
    // volk leaves the entry points null when the extension is absent, so this
    // is the honest test rather than what was asked for.
    // Captures split and reset the frame command buffer for synchronous
    // readback. Keep optional debug labels off in normal runs: a scope that
    // outlives that split would otherwise try to close on the reset buffer.
    // Validation and profiling runs explicitly retain the labels.
    _debugUtils = (validation || debugLabels) && debugUtils &&
                  vkSetDebugUtilsObjectNameEXT != nullptr;

    if (!SDL_Vulkan_CreateSurface(window, _instance.instance, nullptr, &_surface)) {
        throw std::runtime_error("Vulkan: surface creation failed: " +
                                 std::string(SDL_GetError()));
    }

    // Selecting against the surface rules out any device that cannot present to
    // this window, which on a laptop with switchable graphics is a real case.
    // Slang lowers SV_VertexID and SV_InstanceID to VertexIndex/InstanceIndex
    // adjusted by BaseVertex/BaseInstance, which needs the DrawParameters
    // capability. The removed OpenGL SPIR-V path could not read these builtins;
    // here it is simply a feature to ask for.
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
#ifdef R_ENABLE_FSR
    // Required subgroup sizes are core in Vulkan 1.3. Enabling the promoted
    // field here avoids chaining the legacy extension feature struct beside
    // Vulkan13Features, which validation forbids.
    features13.subgroupSizeControl = VK_TRUE;
#endif

    // The merged raster draw uses descriptor indexing even when ray tracing is
    // unavailable. Keep device addresses in the optional tracing feature set,
    // but require the bindless image features for the baseline raster device.
    VkPhysicalDeviceVulkan12Features rasterFeatures12 {};
    rasterFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    rasterFeatures12.descriptorIndexing = VK_TRUE;
    rasterFeatures12.runtimeDescriptorArray = VK_TRUE;
    rasterFeatures12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    rasterFeatures12.descriptorBindingPartiallyBound = VK_TRUE;
    rasterFeatures12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    rasterFeatures12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

    // The resolve samples the derived environment maps as cube arrays, which is
    // not a baseline capability.
    VkPhysicalDeviceFeatures features {};
    features.imageCubeArray = VK_TRUE;
    features.geometryShader = VK_TRUE; // fragment SV_PrimitiveID capability
    // The OpenGL backend filters material textures anisotropically, so
    // matching it needs this. Required rather than optional: every device
    // this targets has had it for well over a decade.
    features.samplerAnisotropy = VK_TRUE;
    // Storage images written without a format decoration. The traced output and
    // every NRD channel already relied on this - Slang emits Unknown for an
    // undecorated RWTexture2D - and the raster resolve now writes the scene
    // output, whose swapchain-matching BGRA format has no SPIR-V image-format
    // enum at all and therefore cannot be decorated even in principle.
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    // Keep the raster selection independent of ray tracing. The latter is an
    // optional Vulkan capability, so an otherwise suitable GPU must not become
    // unselectable just because it cannot trace.
    auto configureSelector = [&](vkb::PhysicalDeviceSelector &selector,
                                 const VkPhysicalDeviceFeatures &requiredFeatures) {
        selector.set_surface(_surface)
            .set_minimum_version(1, 3)
            .set_required_features(requiredFeatures)
            .set_required_features_11(features11)
            .set_required_features_12(rasterFeatures12)
            .set_required_features_13(features13);
    };

    vkb::PhysicalDeviceSelector rasterSelector(_instance);
    configureSelector(rasterSelector, features);
    auto physicalResult = rasterSelector.select();
    if (!physicalResult) {
        throw std::runtime_error("Vulkan: no suitable device: " +
                                 physicalResult.error().message());
    }

    auto physicalDevice = physicalResult.value();
    bool rayQueryEnabled = false;

#ifdef R_ENABLE_FSR
    VkPhysicalDeviceVulkan12Features supportedFsr12 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 supportedFsr {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    supportedFsr.pNext = &supportedFsr12;
    vkGetPhysicalDeviceFeatures2(physicalDevice.physical_device, &supportedFsr);
    const auto fsrFeatureSet = fsrFeatures(supportedFsr.features, supportedFsr12);
    _fsrAvailable = fsrFeatureSet.available;
    if (fsrFeatureSet.core.shaderInt16 == VK_TRUE) {
        features.shaderInt16 = VK_TRUE;
        rasterFeatures12.shaderFloat16 = VK_TRUE;
        if (!physicalDevice.enable_features_if_present(fsrFeatureSet.core) ||
            !physicalDevice.enable_extension_features_if_present(fsrFeatureSet.vulkan12)) {
            throw std::runtime_error("Vulkan: failed to enable supported FSR half-precision features");
        }
    }
#else
    _fsrAvailable = false;
#endif

    const auto rayQueryFeatureSet = rayQueryFeatures(features, rasterFeatures12);

#ifdef R_ENABLE_FSR
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physicalDevice.physical_device, nullptr, &extensionCount,
                                         nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(physicalDevice.physical_device, nullptr, &extensionCount,
                                         extensions.data());
    const bool subgroupSizeControlAvailable = std::any_of(
        extensions.begin(), extensions.end(), [](const VkExtensionProperties &extension) {
            return std::strcmp(extension.extensionName,
                               VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME) == 0;
        });
#endif

    // add_required_extension_features gives vk-bootstrap the feature pNext
    // chain and makes DeviceBuilder enable it with the matching extensions.
    // Selection is deliberately a second, optional pass: failure leaves the
    // raster-selected device intact.
    vkb::PhysicalDeviceSelector rayQuerySelector(_instance);
    configureSelector(rayQuerySelector, rayQueryFeatureSet.core);
    // Do not let an optional feature change the GPU chosen for rasterization.
    rayQuerySelector.set_name(physicalDevice.name);
    for (const auto *extension : rayQueryFeatureSet.extensions)
        rayQuerySelector.add_required_extension(extension);
    rayQuerySelector
        .set_required_features_12(rayQueryFeatureSet.vulkan12)
        .add_required_extension_features(rayQueryFeatureSet.accelerationStructure)
        .add_required_extension_features(rayQueryFeatureSet.rayQuery)
        .add_required_extension_features(rayQueryFeatureSet.rayTracingPipeline);
#ifdef R_ENABLE_FSR
    // FSR2 requests an explicit subgroup size whenever this extension is
    // advertised. Its pipeline pNext is only legal when the matching feature
    // was enabled at device creation.
    if (subgroupSizeControlAvailable) {
        rayQuerySelector.add_required_extension(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    }
#endif
    auto rayQueryPhysicalResult = rayQuerySelector.select();
    if (rayQueryPhysicalResult) {
        physicalDevice = rayQueryPhysicalResult.value();
        rayQueryEnabled = true;
    } else {
        info("Vulkan: ray query unavailable; continuing with raster: " +
                 rayQueryPhysicalResult.error().message(),
             LogChannel::Graphics);
    }

#ifdef R_ENABLE_DLSS
    // After the ray-query selection has settled which physical device this is,
    // and optional at every step: a missing extension costs DLSS, never the
    // device. Measured requirements on an RTX 5090 are five device extensions
    // and three 1.2 features, of which descriptorIndexing and
    // bufferDeviceAddress are already enabled for the tracer.
    if (_streamline.available()) {
        bool ready = true;
        for (const auto *extension : _streamline.deviceExtensions()) {
            if (!physicalDevice.enable_extension_if_present(extension)) {
                info(std::string("Vulkan: DLSS wants device extension ") + extension +
                         ", which this driver does not advertise; DLSS unavailable",
                     LogChannel::Graphics);
                ready = false;
            }
        }
        const auto &names12 = _streamline.features12();
        if (ready && !names12.empty()) {
            auto dlssFeatures12 = sl::getVkPhysicalDeviceVulkan12Features(
                static_cast<uint32_t>(names12.size()),
                const_cast<const char **>(names12.data()));
            if (!physicalDevice.enable_extension_features_if_present(dlssFeatures12)) {
                info("Vulkan: DLSS 1.2 feature requirements unmet; DLSS unavailable",
                     LogChannel::Graphics);
                ready = false;
            }
        }
        _dlssRrAvailable =
            ready && _streamline.supportsRayReconstruction(physicalDevice.physical_device);
    }
#endif

    const auto &logicalDeviceCoreFeatures =
        rayQueryEnabled ? rayQueryFeatureSet.core : features;
    auto logicalPhysicalDevice = prepareLogicalDevice(
        std::move(physicalDevice), logicalDeviceCoreFeatures);
    auto deviceResult = vkb::DeviceBuilder(logicalPhysicalDevice).build();
    if (!deviceResult) {
        throw std::runtime_error("Vulkan: logical device creation failed: " +
                                 deviceResult.error().message());
    }
    _device = deviceResult.value();
    volkLoadDevice(_device.device);

    // All Vulkan entry points, including KHR acceleration-structure commands,
    // are resolved by volk. Do not add a second loader here: the project is
    // intentionally built without linked Vulkan prototypes.
    _rayQueryAvailable = rayQueryEnabled &&
                         vkCreateAccelerationStructureKHR != nullptr &&
                         vkDestroyAccelerationStructureKHR != nullptr &&
                         vkGetAccelerationStructureBuildSizesKHR != nullptr &&
                         vkCmdBuildAccelerationStructuresKHR != nullptr &&
                         vkGetAccelerationStructureDeviceAddressKHR != nullptr &&
                         vkCreateRayTracingPipelinesKHR != nullptr &&
                         vkGetRayTracingShaderGroupHandlesKHR != nullptr &&
                         vkCmdTraceRaysKHR != nullptr;
    if (rayQueryEnabled && !_rayQueryAvailable) {
        info("Vulkan: ray tracing extensions enabled but volk did not load all "
             "required entry points; continuing with raster",
             LogChannel::Graphics);
    }

    auto queueResult = _device.get_queue(vkb::QueueType::graphics);
    if (!queueResult) {
        throw std::runtime_error("Vulkan: no graphics queue: " +
                                 queueResult.error().message());
    }
    _graphicsQueue = queueResult.value();
    _graphicsQueueFamily = _device.get_queue_index(vkb::QueueType::graphics).value();

#ifdef R_ENABLE_DLSS
    // The device exists now, so Streamline can be told about it. Index 0 of the
    // graphics family: DLSS-RR asks for no extra queue, so there is no second
    // one, and naming a family with no queue in it kills the process inside NGX
    // rather than returning an error.
    if (_dlssRrAvailable) {
        _dlssRrAvailable = _streamline.setVulkanInfo(
            _instance.instance, _device.physical_device, _device.device, _graphicsQueueFamily, 0);
    }
    if (_dlssRrAvailable) {
        // Both versions, because the second is the user's to change: the model
        // is nvngx_dlssd.dll beside the executable, and swapping it is how a
        // newer DLSS reaches an already-built engine. A log line is what makes
        // that swap verifiable without opening the settings panel.
        const auto version = _streamline.rayReconstructionVersion();
        info("Vulkan: DLSS Ray Reconstruction available - Streamline " +
                 std::to_string(version.versionSL.major) + "." +
                 std::to_string(version.versionSL.minor) + "." +
                 std::to_string(version.versionSL.build) + ", NGX " +
                 std::to_string(version.versionNGX.major) + "." +
                 std::to_string(version.versionNGX.minor) + "." +
                 std::to_string(version.versionNGX.build),
             LogChannel::Graphics);
    }
#endif

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
    if (rayQueryEnabled) {
        // VMA must know allocations can have device addresses before any later
        // vkGetBufferDeviceAddress use; validation does not reliably catch it.
        allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }
    if (vmaCreateAllocator(&allocatorInfo, &_allocator) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: allocator creation failed");
    }

    VkPhysicalDeviceProperties props {};
    vkGetPhysicalDeviceProperties(_device.physical_device, &props);
    _deviceName = props.deviceName;
    _uniformAlignment = props.limits.minUniformBufferOffsetAlignment;
    _maxAnisotropy = props.limits.maxSamplerAnisotropy;
    info("Vulkan device: " + _deviceName);
    {
        VkPhysicalDeviceProperties2 properties2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        VkPhysicalDeviceDescriptorIndexingProperties descriptorIndexingProperties {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
        properties2.pNext = &descriptorIndexingProperties;
        if (_rayQueryAvailable) {
            _accelerationStructureProperties = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
            _accelerationStructureProperties.pNext = &descriptorIndexingProperties;
            properties2.pNext = &_accelerationStructureProperties;
        }
        vkGetPhysicalDeviceProperties2(_device.physical_device, &properties2);
        _maxBindlessSampledImages = std::min({4096u,
            descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSampledImages,
            descriptorIndexingProperties.maxPerStageDescriptorUpdateAfterBindSampledImages});
        if (_maxBindlessSampledImages == 0) {
            throw std::runtime_error("Vulkan: device has no update-after-bind sampled-image capacity");
        }
    }
    if (_rayQueryAvailable) {
        info("Vulkan ray-query acceleration-structure properties: maxGeometryCount=" +
                 std::to_string(_accelerationStructureProperties.maxGeometryCount) +
                 ", maxInstanceCount=" +
                 std::to_string(_accelerationStructureProperties.maxInstanceCount) +
                 ", minScratchOffsetAlignment=" +
                 std::to_string(
                     _accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment) +
                 ", bindless sampled images=" +
                 std::to_string(_maxBindlessSampledImages),
             LogChannel::Graphics);
    }
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
#ifdef R_ENABLE_DLSS
    // Before the device goes, not after: Streamline still holds it, and the
    // reverse order is an access violation inside SL rather than an error.
    _streamline.shutdown();
    _dlssRrAvailable = false;
#endif
    vkb::destroy_device(_device);
    if (_surface != VK_NULL_HANDLE) {
        vkb::destroy_surface(_instance, _surface);
        _surface = VK_NULL_HANDLE;
    }
    vkb::destroy_instance(_instance);
    _fsrAvailable = false;
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
