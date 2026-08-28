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
#include <glm/vec2.hpp>

#include <memory>

#include "reone/graphics/rhi/upscaler.h"
#include "reone/graphics/vulkan/streamline.h"

namespace reone::graphics {

class VulkanDevice;

/**
 * DLSS Ray Reconstruction, occupying the anti-aliasing slot in place of FSR.
 *
 * One network doing two jobs: it replaces the temporal resolve AND the
 * denoiser, so where this runs, NRD does not. That is not a configuration
 * choice but the shape of the thing - RR takes the noisy, MODULATED radiance
 * and the material factors it was modulated by, and reconstructs from them.
 * Which is exactly what assembleRadiance already produces, so the RR path is
 * the ordinary composite with the denoiser out of the picture, handed here
 * instead of to FSR.
 *
 * Available only where the user supplied Streamline's DLLs and the adapter is
 * an RTX part. ScenePipeline asks VulkanDevice::dlssRrAvailable() before
 * building one and falls back to FSR otherwise; nothing here throws for the
 * ordinary absence.
 */
class DlssRrResolver : public IUpscaler, boost::noncopyable {
public:
    DlssRrResolver(VulkanDevice &device, glm::ivec2 renderExtent, glm::ivec2 displayExtent) :
        _device(device), _renderExtent(renderExtent), _displayExtent(displayExtent) {}

    ~DlssRrResolver() override { deinit(); }

    /** Resolve the feature functions and set the mode. Throws if RR is absent. */
    void init();
    void deinit();
    bool inited() const { return _inited; }

    /**
     * @param jitter this frame's sub-pixel offset in pixels, y down - the same
     *               value FSR is given, from the same generator
     * @param sharpness ignored. DLSSDOptions carries a sharpness field, but it
     *                  is inert - see pushOptions. RenderPipeline routes the
     *                  dial to the postprocess unsharp mask for DLSS frames.
     * @param reset true on the first frame and after a camera cut
     */
    void dispatch(ICommandBuffer &commandBuffer, const UpscalerInputs &inputs,
                  const glm::vec2 &jitter,
                  float frameTimeSeconds, float cameraNear, float cameraFar, float verticalFov,
                  float sharpness, bool reset) override;

private:
    /** Send the mode and guide layout. Configuration, not a per-frame call. */
    bool pushOptions();

    VulkanDevice &_device;
    glm::ivec2 _renderExtent;
    glm::ivec2 _displayExtent;
    bool _inited {false};

    /**
     * Streamline counts frames itself through a token; this is the index handed
     * to slGetNewFrameToken, which must advance by one per frame.
     */
    uint32_t _frameIndex {0};

    PFun_slDLSSDSetOptions *_setOptions {nullptr};
    PFun_slDLSSDGetOptimalSettings *_getOptimalSettings {nullptr};
};

} // namespace reone::graphics

#endif
