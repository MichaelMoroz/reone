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

#ifdef R_ENABLE_DLSS

#include <boost/noncopyable.hpp>

#include <vector>

// volk owns the Vulkan entry points; Streamline's Vulkan helper header names
// VkDevice and friends without including a Vulkan header of its own.
#include <volk.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_dlss_d.h>
#include <sl_helpers_vk.h>

namespace reone::graphics {

/**
 * Streamline's lifetime, and the one place that knows the interposer exists.
 *
 * Compiled unconditionally, unlike the NRD denoiser beside it: Streamline's
 * headers are MIT and carry nothing from the NGX SDK vendored next to them, so
 * this links into a GPL binary where NRD cannot. Nothing here is
 * redistributed. The three DLLs it needs - sl.interposer.dll, sl.dlss_d.dll,
 * nvngx_dlssd.dll - are the user's to place beside the executable, and every
 * entry point below degrades to "unavailable" when they are absent. A machine
 * without them takes exactly the path it took before this existed.
 *
 * MANUAL HOOKING, deliberately. Streamline's default mode replaces the Vulkan
 * loader by interposing vkGetInstanceProcAddr, which is the collision class
 * that already cost this project twice (see the NRI note in nrddenoiser.h).
 * Manual hooking leaves volk owning vulkan-1.dll outright and touches only the
 * eight entry points sl_hooks.h names. Measured on an RTX 5090: of ten probed
 * functions, six diverge - all of them swapchain, present or waitIdle - and
 * vkCmdDispatch, vkCmdPipelineBarrier2, vkCreateComputePipelines and
 * vkQueueSubmit2 come back as byte-identical pointers.
 */
class StreamlineRuntime : boost::noncopyable {
public:
    ~StreamlineRuntime() { shutdown(); }

    /**
     * Load the interposer and slInit, requesting DLSS-RR.
     *
     * MUST be called before the VkDevice is created: Streamline requires it,
     * and slGetFeatureRequirements has to be answered before the device
     * builder runs so its extensions can be enabled.
     *
     * Returns false for every ordinary reason the feature is not there - no
     * DLLs, no driver, not an RTX part - and never throws for them. A false
     * here is not an error, it is the common case.
     */
    bool init();

    /**
     * slShutdown. MUST run before vkDestroyDevice: Streamline still holds the
     * device, and destroying it first is an access violation inside SL rather
     * than an error return.
     */
    void shutdown();

    bool available() const { return _inited; }

    /**
     * What DLSS-RR needs at device creation. Measured empty of queues on an
     * RTX 5090 - 0 graphics, 0 compute, 0 optical-flow - so unlike DLSS-G this
     * asks for no extra queue beyond the application's own.
     */
    const std::vector<const char *> &instanceExtensions() const { return _instanceExtensions; }
    const std::vector<const char *> &deviceExtensions() const { return _deviceExtensions; }
    /** Names from FeatureRequirements::vkFeatures12, to fold into the selector. */
    const std::vector<const char *> &features12() const { return _features12; }

    /**
     * Versions of the DLSS_RR plugin actually loaded: Streamline's own, and the
     * NGX model behind it. Queried rather than assumed, because the second is
     * the user's to replace.
     */
    sl::FeatureVersion rayReconstructionVersion() const;

    /** Whether this adapter can run Ray Reconstruction. Valid after init(). */
    bool supportsRayReconstruction(VkPhysicalDevice physicalDevice) const;

    /**
     * Hand Streamline the device volk created. Only name queue families that
     * actually had a queue created in them - naming an empty one kills the
     * process inside NGX rather than returning an error.
     */
    bool setVulkanInfo(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                       uint32_t graphicsQueueFamily, uint32_t graphicsQueueIndex);

    /**
     * The per-frame entry points, resolved once. Public because the resolver
     * is the only caller and a forwarding method per function would be noise.
     * Null whenever available() is false.
     */
    PFun_slGetNewFrameToken *getNewFrameToken {nullptr};
    PFun_slSetConstants *setConstants {nullptr};
    PFun_slSetTagForFrame *setTagForFrame {nullptr};
    PFun_slEvaluateFeature *evaluateFeature {nullptr};
    PFun_slGetFeatureFunction *getFeatureFunction {nullptr};

private:
    void *_module {nullptr};
    bool _inited {false};

    PFun_slInit *_init {nullptr};
    PFun_slShutdown *_shutdown {nullptr};
    PFun_slIsFeatureSupported *_isFeatureSupported {nullptr};
    PFun_slGetFeatureRequirements *_getFeatureRequirements {nullptr};
    PFun_slSetVulkanInfo *_setVulkanInfo {nullptr};
    PFun_slGetFeatureVersion *_getFeatureVersion {nullptr};

    std::vector<const char *> _instanceExtensions;
    std::vector<const char *> _deviceExtensions;
    std::vector<const char *> _features12;
};

} // namespace reone::graphics

#endif
