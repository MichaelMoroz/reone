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

#include "reone/graphics/vulkan/streamline.h"

#ifdef R_ENABLE_DLSS

#include <sl_helpers.h>

#include <windows.h>

#include <cstdlib>
#include <string>

#include "reone/system/logutil.h"

namespace reone::graphics {

namespace {

/**
 * A GUID, and it has to be one.
 *
 * Streamline documents projectId as a GUID and NGX enforces it: a
 * human-readable string comes back as NVSDK_NGX_Result_FAIL_InvalidParameter
 * (0xbad00005), which sl.dlss_d then reports as "DLSSD feature is not
 * supported. Please check if you have a valid nvngx_dlssd.dll or your driver
 * is supporting DLSSD". That message names the DLL and the driver, neither of
 * which is the problem, so do not replace this with something readable.
 */
constexpr const char *kProjectId = "7c8f1a2e-63d4-4b95-9a17-2e5f80c6d431";

void logCallback(sl::LogType type, const char *message) {
    std::string text = std::string("Streamline: ") + message;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    switch (type) {
    case sl::LogType::eError:
        warn(text, LogChannel::Graphics);
        break;
    case sl::LogType::eWarn:
        warn(text, LogChannel::Graphics);
        break;
    default:
        debug(text, LogChannel::Graphics);
        break;
    }
}

template <typename T>
T *entryPoint(HMODULE module, const char *name) {
    return reinterpret_cast<T *>(GetProcAddress(module, name));
}

} // namespace

bool StreamlineRuntime::init() {
    if (_inited) {
        return true;
    }
    // Beside the executable, by the default search order. Absent is the common
    // case and not worth a warning: a user who has not supplied the DLLs has
    // not asked for this.
    HMODULE module = LoadLibraryW(L"sl.interposer.dll");
    if (!module) {
        debug("Streamline: sl.interposer.dll not found; DLSS unavailable",
              LogChannel::Graphics);
        return false;
    }
    _module = module;

    _init = entryPoint<PFun_slInit>(module, "slInit");
    _shutdown = entryPoint<PFun_slShutdown>(module, "slShutdown");
    _isFeatureSupported = entryPoint<PFun_slIsFeatureSupported>(module, "slIsFeatureSupported");
    _getFeatureRequirements =
        entryPoint<PFun_slGetFeatureRequirements>(module, "slGetFeatureRequirements");
    _setVulkanInfo = entryPoint<PFun_slSetVulkanInfo>(module, "slSetVulkanInfo");
    _getFeatureVersion = entryPoint<PFun_slGetFeatureVersion>(module, "slGetFeatureVersion");
    getNewFrameToken = entryPoint<PFun_slGetNewFrameToken>(module, "slGetNewFrameToken");
    setConstants = entryPoint<PFun_slSetConstants>(module, "slSetConstants");
    setTagForFrame = entryPoint<PFun_slSetTagForFrame>(module, "slSetTagForFrame");
    evaluateFeature = entryPoint<PFun_slEvaluateFeature>(module, "slEvaluateFeature");
    getFeatureFunction = entryPoint<PFun_slGetFeatureFunction>(module, "slGetFeatureFunction");
    if (!_init || !_shutdown || !_isFeatureSupported || !_getFeatureRequirements ||
        !_setVulkanInfo || !getNewFrameToken || !setConstants || !setTagForFrame ||
        !evaluateFeature || !getFeatureFunction) {
        warn("Streamline: sl.interposer.dll is missing entry points; DLSS unavailable",
             LogChannel::Graphics);
        shutdown();
        return false;
    }

    sl::Feature features[] = {sl::kFeatureDLSS_RR};
    sl::Preferences pref {};
    // Streamline's own log, off unless asked for. Its failures name a reason
    // that nothing on our side can reconstruct - slSetTagForFrame returns one
    // eErrorX for a dozen distinct causes - so this is the difference between
    // diagnosing a tagging fault in one run and guessing at it. Set
    // REONE_SL_LOG=1 (or 2 for verbose) to route it into the Graphics channel.
    const char *logEnv = std::getenv("REONE_SL_LOG");
    const int logWanted = logEnv ? std::atoi(logEnv) : 0;
    pref.logLevel = logWanted >= 2   ? sl::LogLevel::eVerbose
                    : logWanted >= 1 ? sl::LogLevel::eDefault
                                     : sl::LogLevel::eOff;
    pref.logMessageCallback = logCallback;
    pref.pathToLogsAndData = nullptr;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    // ASSIGNED, not or-ed into the default. The default carries eAllowOTA and
    // eLoadDownloadedPlugins, under which Streamline prefers a plugin from the
    // driver's cache over the one beside the executable - observed loading
    // sl.dlss_d 2.12.129 over the 2.12.0 these headers describe, and calling
    // across that ABI gap was an access violation rather than an error return.
    // Pinning to the shipped plugin costs nothing a user wants: the RR model
    // itself lives in nvngx_dlssd.dll, which this flag does not govern, so
    // dropping in a newer one still works.
    //
    // eUseFrameBasedResourceTagging is what makes slSetTagForFrame legal at
    // all: without it every call returns an error naming this flag, which is
    // the one helpful message in the set.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "reone";
    pref.projectId = kProjectId;
    pref.renderAPI = sl::RenderAPI::eVulkan;

    sl::Result result = _init(pref, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        info(std::string("Streamline: slInit failed (") + sl::getResultAsStr(result) +
                 "); DLSS unavailable",
             LogChannel::Graphics);
        shutdown();
        return false;
    }
    _inited = true;

    // Answered before any Vulkan object exists, which is the point: the device
    // builder needs these lists.
    sl::FeatureRequirements requirements {};
    if (_getFeatureRequirements(sl::kFeatureDLSS_RR, requirements) == sl::Result::eOk) {
        for (uint32_t i = 0; i < requirements.vkNumInstanceExtensions; ++i) {
            _instanceExtensions.push_back(requirements.vkInstanceExtensions[i]);
        }
        for (uint32_t i = 0; i < requirements.vkNumDeviceExtensions; ++i) {
            _deviceExtensions.push_back(requirements.vkDeviceExtensions[i]);
        }
        for (uint32_t i = 0; i < requirements.vkNumFeatures12; ++i) {
            _features12.push_back(requirements.vkFeatures12[i]);
        }
    }
    return true;
}

void StreamlineRuntime::shutdown() {
    if (_inited && _shutdown) {
        _shutdown();
    }
    _inited = false;
    if (_module) {
        FreeLibrary(static_cast<HMODULE>(_module));
        _module = nullptr;
    }
    _init = nullptr;
    _shutdown = nullptr;
    _isFeatureSupported = nullptr;
    _getFeatureRequirements = nullptr;
    _setVulkanInfo = nullptr;
    _getFeatureVersion = nullptr;
    getNewFrameToken = nullptr;
    setConstants = nullptr;
    setTagForFrame = nullptr;
    evaluateFeature = nullptr;
    getFeatureFunction = nullptr;
    _instanceExtensions.clear();
    _deviceExtensions.clear();
    _features12.clear();
}

sl::FeatureVersion StreamlineRuntime::rayReconstructionVersion() const {
    sl::FeatureVersion version {};
    if (_inited && _getFeatureVersion) {
        _getFeatureVersion(sl::kFeatureDLSS_RR, version);
    }
    return version;
}

bool StreamlineRuntime::supportsRayReconstruction(VkPhysicalDevice physicalDevice) const {
    if (!_inited || !physicalDevice) {
        return false;
    }
    sl::AdapterInfo adapter {};
    adapter.vkPhysicalDevice = physicalDevice;
    const sl::Result result = _isFeatureSupported(sl::kFeatureDLSS_RR, adapter);
    if (result != sl::Result::eOk) {
        info(std::string("Streamline: DLSS Ray Reconstruction unsupported on this adapter (") +
                 sl::getResultAsStr(result) + ")",
             LogChannel::Graphics);
        return false;
    }
    return true;
}

bool StreamlineRuntime::setVulkanInfo(VkInstance instance, VkPhysicalDevice physicalDevice,
                                      VkDevice device, uint32_t graphicsQueueFamily,
                                      uint32_t graphicsQueueIndex) {
    if (!_inited) {
        return false;
    }
    sl::VulkanInfo vkInfo {};
    vkInfo.instance = instance;
    vkInfo.physicalDevice = physicalDevice;
    vkInfo.device = device;
    vkInfo.graphicsQueueFamily = graphicsQueueFamily;
    vkInfo.graphicsQueueIndex = graphicsQueueIndex;
    // The same family and index, not a compute family this device never
    // created a queue in - see the header. DLSS-RR asks for no extra queue, so
    // there is never a second one to name.
    vkInfo.computeQueueFamily = graphicsQueueFamily;
    vkInfo.computeQueueIndex = graphicsQueueIndex;
    vkInfo.useNativeOpticalFlowMode = true;
    const sl::Result result = _setVulkanInfo(vkInfo);
    if (result != sl::Result::eOk) {
        warn(std::string("Streamline: slSetVulkanInfo failed (") + sl::getResultAsStr(result) +
                 "); DLSS unavailable",
             LogChannel::Graphics);
        return false;
    }
    return true;
}

} // namespace reone::graphics

#endif
