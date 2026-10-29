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

#include "reone/graphics/vulkan/dlssrr.h"

#ifdef R_ENABLE_DLSS

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat4x4.hpp>

#include <sl_helpers.h>

#include "reone/graphics/vulkan/commandbuffer.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/system/logutil.h"

namespace reone::graphics {

namespace {

/**
 * glm to Streamline, which is a memcpy and not an oversight.
 *
 * SL documents its float4x4 as row major, and its matrices are the row-vector
 * form (v * M) that D3D samples use. glm holds the column-vector form (M * v)
 * in column-major storage. Those two disagree twice - once on convention, once
 * on storage - and the disagreements cancel exactly: glm's column i holds the
 * same four floats as the transposed matrix's row i. So the bytes already say
 * what SL wants to read, and transposing here would be the bug.
 */
sl::float4x4 toSl(const glm::mat4 &m) {
    sl::float4x4 out {};
    static_assert(sizeof(out) == sizeof(m), "sl::float4x4 must match glm::mat4 in size");
    std::memcpy(&out, &m, sizeof(out));
    return out;
}

sl::float3 toSl(const glm::vec3 &v) {
    return sl::float3 {v.x, v.y, v.z};
}

/**
 * Wrap one of our images as a Streamline resource.
 *
 * Under manual hooking SL never sees a creation call, so every one of these
 * fields is load-bearing and has to match reality - the layout most of all,
 * because SL transitions from whatever it is told rather than from what is.
 */
sl::Resource toResource(IImage &image, VkImageLayout layout) {
    auto &vk = toVulkanImage(image);
    sl::Resource resource {sl::ResourceType::eTex2d, vk.handle(), nullptr, vk.view(),
                           static_cast<uint32_t>(layout)};
    resource.width = static_cast<uint32_t>(vk.extent().x);
    resource.height = static_cast<uint32_t>(vk.extent().y);
    resource.nativeFormat = static_cast<uint32_t>(vk.format());
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = static_cast<uint32_t>(vk.usage());
    return resource;
}

/**
 * Which DLSS mode a render/display ratio names.
 *
 * Nearest rather than exact: the pipeline rounds its render extent to whole
 * pixels, so 1/1.5 of 3440 comes back as a ratio that is close to but not
 * equal to 0.667, and an equality test would silently fall through to Quality
 * for every mode.
 */
sl::DLSSMode dlssModeForRatio(glm::ivec2 render, glm::ivec2 display) {
    if (render == display) {
        return sl::DLSSMode::eDLAA;
    }
    const float ratio = static_cast<float>(render.x) / static_cast<float>(std::max(1, display.x));
    const std::pair<float, sl::DLSSMode> table[] = {
        {1.0f, sl::DLSSMode::eDLAA},
        {1.0f / 1.5f, sl::DLSSMode::eMaxQuality},
        {1.0f / 1.7f, sl::DLSSMode::eBalanced},
        {1.0f / 2.0f, sl::DLSSMode::eMaxPerformance},
        {1.0f / 3.0f, sl::DLSSMode::eUltraPerformance},
    };
    sl::DLSSMode best = sl::DLSSMode::eMaxQuality;
    float bestDelta = std::numeric_limits<float>::max();
    for (const auto &[value, mode] : table) {
        const float delta = std::abs(ratio - value);
        if (delta < bestDelta) {
            bestDelta = delta;
            best = mode;
        }
    }
    return best;
}

/**
 * Hand out one Streamline viewport per resolver, never reusing an id.
 *
 * Monotonic rather than pooled: Streamline keeps per-viewport history, and
 * recycling an id would hand a new render target the accumulated state of a
 * dead one. Scene pipelines are built on mode and resolution changes, so the
 * count stays in the low tens over a session.
 */
uint32_t nextViewportId() {
    static uint32_t next = 0;
    return next++;
}

} // namespace

void DlssRrResolver::init() {
    if (_inited) {
        return;
    }
    if (!_device.dlssRrAvailable()) {
        throw std::runtime_error("DLSS: Ray Reconstruction unavailable on this device");
    }
    _viewport = nextViewportId();
    auto &slRuntime = _device.streamline();

    void *fn = nullptr;
    if (slRuntime.getFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", fn) !=
            sl::Result::eOk ||
        !fn) {
        throw std::runtime_error("DLSS: slDLSSDSetOptions unavailable");
    }
    _setOptions = reinterpret_cast<PFun_slDLSSDSetOptions *>(fn);
    fn = nullptr;
    if (slRuntime.getFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings", fn) ==
            sl::Result::eOk &&
        fn) {
        _getOptimalSettings = reinterpret_cast<PFun_slDLSSDGetOptimalSettings *>(fn);
    }

    // The render extent is the pipeline's, decided by renderExtentFor and the
    // render scale. RR is told what it is rather than asked what it should be:
    // the tracer, the G-buffer and the channel images are already that size, so
    // an "optimal" figure from the plugin could only disagree with them. The
    // query is kept for the log line, which is worth having when a scale looks
    // wrong.
    if (_getOptimalSettings) {
        sl::DLSSDOptions query {};
        query.outputWidth = static_cast<uint32_t>(_displayExtent.x);
        query.outputHeight = static_cast<uint32_t>(_displayExtent.y);
        query.mode = sl::DLSSMode::eMaxQuality;
        sl::DLSSDOptimalSettings optimal {};
        if (_getOptimalSettings(query, optimal) == sl::Result::eOk) {
            debug("DLSS: MaxQuality for " + std::to_string(_displayExtent.x) + "x" +
                      std::to_string(_displayExtent.y) + " suggests " +
                      std::to_string(optimal.optimalRenderWidth) + "x" +
                      std::to_string(optimal.optimalRenderHeight) + "; rendering at " +
                      std::to_string(_renderExtent.x) + "x" + std::to_string(_renderExtent.y),
                  LogChannel::Graphics);
        }
    }

    if (!pushOptions()) {
        throw std::runtime_error("DLSS: slDLSSDSetOptions failed");
    }
    _inited = true;
    _frameIndex = 0;
}

bool DlssRrResolver::pushOptions() {
    sl::DLSSDOptions options {};
    options.outputWidth = static_cast<uint32_t>(_displayExtent.x);
    options.outputHeight = static_cast<uint32_t>(_displayExtent.y);
    // The mode is derived from the extents the pipeline was built at rather
    // than read from the option: GraphicsOptions::dlssMode already decided the
    // render scale through renderExtentFor, so taking it from the ratio keeps
    // the two from disagreeing about the same number.
    options.mode = dlssModeForRatio(_renderExtent, _displayExtent);
    // Always zero, and deliberately not wired to the sharpness dial.
    //
    // The field is here and slDLSSDSetOptions accepts any value in range, but
    // it is inert: measured on an RTX 5090 with Streamline 2.12.0 / NGX
    // 310.7.0, 0.0 against 1.0 moved the mean image gradient by 0.004%. Only
    // the super-resolution struct admits this in the type system
    // (SR_DEPRECATED_SHARPENING); Ray Reconstruction kept the field without the
    // attribute. RenderPipeline sends the dial to the unsharp mask instead.
    //
    // Kept at zero rather than deleted so that if a later NGX revives it, this
    // is one line and a re-measurement rather than a rediscovery.
    options.sharpness = 0.0f;
    // One image carrying world normal in xyz and linear roughness in w - the
    // channel the tracer already wrote for NRD, whose RGBA16_SNORM encoding
    // takes both unpacked, and which the PBR provider now writes too.
    options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
    const sl::Result result = _setOptions(sl::ViewportHandle {_viewport}, options);
    if (result != sl::Result::eOk) {
        warn(std::string("DLSS: slDLSSDSetOptions failed: ") + sl::getResultAsStr(result),
             LogChannel::Graphics);
        return false;
    }
    return true;
}

void DlssRrResolver::deinit() {
    // Release this viewport's history and internal targets. Without it every
    // pipeline rebuild - a resolution change, a mode switch - would strand a
    // viewport's worth of DLSS state for the life of the process.
    if (_inited && _device.dlssRrAvailable()) {
        auto &slRuntime = _device.streamline();
        if (slRuntime.freeResources) {
            slRuntime.freeResources(sl::kFeatureDLSS_RR, sl::ViewportHandle {_viewport});
        }
    }
    _inited = false;
    _setOptions = nullptr;
    _getOptimalSettings = nullptr;
}

void DlssRrResolver::dispatch(ICommandBuffer &commandBuffer, const UpscalerInputs &inputs,
                              const glm::vec2 &jitter, float frameTimeSeconds, float cameraNear,
                              float cameraFar, float verticalFov, float sharpness, bool reset) {
    if (!_inited || !inputs.color || !inputs.depth || !inputs.motion || !inputs.output) {
        return;
    }
    auto &slRuntime = _device.streamline();
    const auto cmd = toVulkanCommandBuffer(commandBuffer).handle();


    sl::FrameToken *frame = nullptr;
    if (slRuntime.getNewFrameToken(frame, &_frameIndex) != sl::Result::eOk || !frame) {
        warn("DLSS: no frame token this frame; skipping", LogChannel::Graphics);
        return;
    }
    ++_frameIndex;

    // clip -> world -> previous clip. The matrices arrive unjittered at both
    // ends, because SL takes the sub-pixel offset separately below and folding
    // it into a matrix would count it twice.
    const glm::mat4 viewProjection = inputs.projection * inputs.view;
    const glm::mat4 prevViewProjection = inputs.prevProjection * inputs.prevView;
    const glm::mat4 clipToPrevClip = prevViewProjection * glm::inverse(viewProjection);

    sl::Constants constants {};
    constants.cameraViewToClip = toSl(inputs.projection);
    constants.clipToCameraView = toSl(glm::inverse(inputs.projection));
    constants.clipToPrevClip = toSl(clipToPrevClip);
    constants.prevClipToClip = toSl(glm::inverse(clipToPrevClip));
    constants.jitterOffset = sl::float2 {jitter.x, jitter.y};
    // Explicitly zero rather than left at its INVALID_FLOAT default, which SL
    // validates and complains about every frame. There is no pinhole offset
    // here: the jitter above is the only sub-pixel displacement in the
    // projection.
    constants.cameraPinholeOffset = sl::float2 {0.0f, 0.0f};
    // The G-buffer stores current minus previous as half a clip-space delta
    // with y up, which is already a UV-space delta - so normalizing it to the
    // [-1,1] SL asks for is a sign per axis and no magnitude. Both corrections
    // ride here rather than in a rewrite of an attachment several passes
    // already read, exactly as the FSR scale beside it does.
    //
    // UNVERIFIED against a moving camera. If ghosting trails the wrong way
    // during bring-up, this is the first line to doubt.
    constants.mvecScale = sl::float2 {-1.0f, 1.0f};
    constants.cameraPos = toSl(inputs.cameraPosition);
    // Rows of the view matrix are the camera basis in world space.
    const glm::mat4 viewInv = glm::inverse(inputs.view);
    constants.cameraRight = toSl(glm::vec3(viewInv[0]));
    constants.cameraUp = toSl(glm::vec3(viewInv[1]));
    // Negated: the view matrix looks down -z, and SL wants the direction the
    // camera faces.
    constants.cameraFwd = toSl(-glm::vec3(viewInv[2]));
    constants.cameraNear = cameraNear;
    constants.cameraFar = cameraFar;
    constants.cameraFOV = verticalFov;
    constants.cameraAspectRatio = static_cast<float>(_renderExtent.x) /
                                  static_cast<float>(std::max(1, _renderExtent.y));
    constants.depthInverted = sl::Boolean::eFalse;
    // The G-buffer's vectors are the full screen-space delta, camera motion
    // included, so SL must not try to add it back.
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    // Built from the unjittered matrices at both ends - see the tracer's
    // setUpscalerGuides, which is deliberate about this.
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    if (slRuntime.setConstants(constants, *frame, sl::ViewportHandle {_viewport}) != sl::Result::eOk) {
        warn("DLSS: slSetConstants failed; skipping this frame", LogChannel::Graphics);
        return;
    }

    // The layouts are the ones ScenePipeline::upscalePass has already put these
    // images into: everything sampled but the output, which is General.
    auto colorRes = toResource(*inputs.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    auto depthRes = toResource(*inputs.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    auto motionRes = toResource(*inputs.motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    auto outputRes = toResource(*inputs.output, VK_IMAGE_LAYOUT_GENERAL);

    const sl::Extent renderExtent {0, 0, static_cast<uint32_t>(_renderExtent.x),
                                     static_cast<uint32_t>(_renderExtent.y)};
    const sl::Extent displayExtent {0, 0, static_cast<uint32_t>(_displayExtent.x),
                                      static_cast<uint32_t>(_displayExtent.y)};

    std::vector<sl::ResourceTag> tags;
    tags.reserve(7);
    // eValidUntilEvaluate rather than eValidUntilPresent: these are pipeline
    // intermediates that later passes overwrite within the same frame, so SL
    // may not hold references to them past the evaluate below.
    tags.emplace_back(&colorRes, sl::kBufferTypeScalingInputColor,
                      sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent);
    tags.emplace_back(&outputRes, sl::kBufferTypeScalingOutputColor,
                      sl::ResourceLifecycle::eValidUntilEvaluate, &displayExtent);
    tags.emplace_back(&depthRes, sl::kBufferTypeDepth,
                      sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent);
    tags.emplace_back(&motionRes, sl::kBufferTypeMotionVectors,
                      sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent);

    // The guides beyond the four required tags. Absent in Retro, which shades
    // into no channel at all - RR is not offered there, but tag defensively
    // rather than trusting a caller.
    sl::Resource albedoRes {};
    sl::Resource specAlbedoRes {};
    sl::Resource normalRoughnessRes {};
    if (inputs.diffuseAlbedo) {
        albedoRes = toResource(*inputs.diffuseAlbedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        tags.emplace_back(&albedoRes, sl::kBufferTypeAlbedo,
                          sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent);
    }
    if (inputs.specularAlbedo) {
        specAlbedoRes =
            toResource(*inputs.specularAlbedo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        tags.emplace_back(&specAlbedoRes, sl::kBufferTypeSpecularAlbedo,
                          sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent);
    }
    if (inputs.normalRoughness) {
        normalRoughnessRes =
            toResource(*inputs.normalRoughness, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        tags.emplace_back(&normalRoughnessRes, sl::kBufferTypeNormalRoughness,
                          sl::ResourceLifecycle::eValidUntilEvaluate, &renderExtent);
    }

    if (slRuntime.setTagForFrame(*frame, sl::ViewportHandle {_viewport}, tags.data(),
                          static_cast<uint32_t>(tags.size()), cmd) != sl::Result::eOk) {
        warn("DLSS: slSetTagForFrame failed; skipping this frame", LogChannel::Graphics);
        return;
    }

    const sl::ViewportHandle viewport {_viewport};
    const sl::BaseStructure *evalInputs[] = {&viewport};
    const sl::Result result = slRuntime.evaluateFeature(sl::kFeatureDLSS_RR, *frame, evalInputs, 1,
                                                   cmd);
    if (result != sl::Result::eOk) {
        warn(std::string("DLSS: slEvaluateFeature failed: ") + sl::getResultAsStr(result),
             LogChannel::Graphics);
    }
    // SL leaves the command buffer's bound state as its own passes left it.
    // Every recorder downstream binds its pipeline and descriptor sets before
    // dispatching, so there is nothing to restore here - unlike an engine that
    // relies on state persisting across a pass boundary.
}

} // namespace reone::graphics

#endif
