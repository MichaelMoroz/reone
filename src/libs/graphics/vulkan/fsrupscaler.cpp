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

#include "reone/graphics/vulkan/fsrupscaler.h"

#include <stdexcept>
#include <string>
#include <cwchar>

#include <volk.h>

#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/commandbuffer.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone::graphics {

namespace {

/**
 * Wrap one of our images for FSR. The state must match reality or the backend
 * inserts the wrong barrier: every image the trace pipeline touches lives in
 * GENERAL, which is what FFX_RESOURCE_STATE_UNORDERED_ACCESS maps to.
 */
FfxResource wrapImage(FfxFsr2Context *context, VulkanImage &image, const wchar_t *name) {
    return ffxGetTextureResourceVK(context, image.handle(), image.view(),
                                   static_cast<uint32_t>(image.extent().x),
                                   static_cast<uint32_t>(image.extent().y), image.format(), name,
                                   FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}

void fsrMessage(FfxFsr2MsgType, const wchar_t *message) {
    if (!message) {
        return;
    }
    warn("FSR: " + std::string(message, message + std::wcslen(message)), LogChannel::Graphics);
}

} // namespace

FsrUpscaler::FsrUpscaler(VulkanDevice &device, glm::ivec2 extent) :
    _device(device),
    _extent(extent) {
}

void FsrUpscaler::init() {
    if (_inited) {
        return;
    }
    _scratch.resize(ffxFsr2GetScratchMemorySizeVK(_device.physicalDevice()));
    if (_scratch.empty()) {
        throw std::runtime_error("FSR: Vulkan backend returned no scratch memory requirement");
    }
    const FfxErrorCode interfaceCode = ffxFsr2GetInterfaceVK(
        &_interface, _scratch.data(), _scratch.size(), _device.physicalDevice(), vkGetDeviceProcAddr);
    if (interfaceCode != FFX_OK) {
        throw std::runtime_error("FSR: failed to initialise Vulkan callbacks: " +
                                 std::to_string(interfaceCode));
    }

    _context = std::make_unique<FfxFsr2Context>();
    FfxFsr2ContextDescription description {};
    description.flags = FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE |
                        FFX_FSR2_ENABLE_AUTO_EXPOSURE |
                        FFX_FSR2_ENABLE_DEBUG_CHECKING;
    description.maxRenderSize = {static_cast<uint32_t>(_extent.x), static_cast<uint32_t>(_extent.y)};
    description.displaySize = description.maxRenderSize;
    description.callbacks = _interface;
    description.device = ffxGetDeviceVK(_device.handle());
    description.fpMessage = fsrMessage;
    const FfxErrorCode createCode = ffxFsr2ContextCreate(_context.get(), &description);
    if (createCode != FFX_OK) {
        _context.reset();
        throw std::runtime_error("FSR: context creation failed: " + std::to_string(createCode));
    }
    _contextCreated = true;
    _inited = true;
}

void FsrUpscaler::deinit() {
    if (_contextCreated) {
        ffxFsr2ContextDestroy(_context.get());
        _contextCreated = false;
    }
    _context.reset();
    _scratch.clear();
    _inited = false;
}

glm::vec2 FsrUpscaler::jitterOffset(int frameIndex, glm::ivec2 extent) {
    const int32_t phaseCount = ffxFsr2GetJitterPhaseCount(extent.x, extent.x);
    float x = 0.0f;
    float y = 0.0f;
    ffxFsr2GetJitterOffset(&x, &y, frameIndex, phaseCount > 0 ? phaseCount : 1);
    return {x, y};
}

void FsrUpscaler::dispatch(ICommandBuffer &commandBuffer, const TracingUpscalerInputs &inputs,
                           const glm::vec2 &jitter,
                           float frameTimeSeconds, float cameraNear, float cameraFar,
                           float verticalFov, float sharpness, bool reset) {
    if (!_inited || !inputs.color || !inputs.depth || !inputs.motion || !inputs.output) {
        return;
    }
    const auto cmd = toVulkanCommandBuffer(commandBuffer).handle();
    FfxFsr2DispatchDescription dispatch {};
    dispatch.commandList = ffxGetCommandListVK(cmd);
    dispatch.color = wrapImage(_context.get(), toVulkanImage(*inputs.color), L"pt_color");
    dispatch.depth = wrapImage(_context.get(), toVulkanImage(*inputs.depth), L"pt_depth");
    dispatch.motionVectors = wrapImage(_context.get(), toVulkanImage(*inputs.motion), L"pt_motion");
    dispatch.output = wrapImage(_context.get(), toVulkanImage(*inputs.output), L"pt_upscaled");
    // Auto exposure is on, so no exposure resource and no reactive masks. The
    // masks are worth revisiting: without them FSR falls back on its own
    // shading-change detection, which AMD says handles transparency "as best it
    // can" - and additive blades are exactly the case it means.
    dispatch.exposure = ffxGetTextureResourceVK(_context.get(), nullptr, nullptr, 1, 1,
                                                 VK_FORMAT_UNDEFINED, L"pt_exposure");
    dispatch.reactive = ffxGetTextureResourceVK(_context.get(), nullptr, nullptr, 1, 1,
                                                 VK_FORMAT_UNDEFINED, L"pt_reactive");
    dispatch.transparencyAndComposition = ffxGetTextureResourceVK(
        _context.get(), nullptr, nullptr, 1, 1, VK_FORMAT_UNDEFINED, L"pt_transparency");

    dispatch.jitterOffset = {jitter.x, jitter.y};
    // The tracer writes a UV-space delta, so the scale is the render size; a
    // pixel-space encoding would want (1,1) and an NDC one (0.5W, -0.5H).
    dispatch.motionVectorScale = {static_cast<float>(_extent.x), static_cast<float>(_extent.y)};
    dispatch.renderSize = {static_cast<uint32_t>(_extent.x), static_cast<uint32_t>(_extent.y)};
    // RCAS runs as an extra pass, so skip it outright at zero rather than
    // paying for a no-op sharpen.
    dispatch.enableSharpening = sharpness > 0.0f;
    dispatch.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    dispatch.frameTimeDelta = frameTimeSeconds * 1000.0f; // FSR wants milliseconds
    dispatch.preExposure = 1.0f;                          // must be > 0; we do not pre-expose
    dispatch.reset = reset;
    // Plain forward Vulkan depth, 0 at the near plane. Neither DEPTH_INVERTED
    // nor DEPTH_INFINITE applies and near/far are passed the obvious way round.
    dispatch.cameraNear = cameraNear;
    dispatch.cameraFar = cameraFar;
    dispatch.cameraFovAngleVertical = verticalFov;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.enableAutoReactive = false;
    dispatch.colorOpaqueOnly = ffxGetTextureResourceVK(_context.get(), nullptr, nullptr, 1, 1,
                                                        VK_FORMAT_UNDEFINED, L"pt_color_opaque");

    const FfxErrorCode code = ffxFsr2ContextDispatch(_context.get(), &dispatch);
    if (code != FFX_OK) {
        warn("FSR: dispatch failed: " + std::to_string(code), LogChannel::Graphics);
    }
}

} // namespace reone::graphics
