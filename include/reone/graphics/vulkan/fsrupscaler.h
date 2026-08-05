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

#pragma once

#include <boost/noncopyable.hpp>
#include <glm/vec2.hpp>

#include <memory>
#include <vector>

#include "reone/graphics/vulkan/image.h"

#include <ffx_fsr2.h>
#include <ffx_fsr2_vk.h>

namespace reone::graphics {

class VulkanDevice;

/**
 * AMD FidelityFX Super Resolution 2.2.1, at NativeAA (1.0x).
 *
 * This is the anti-aliasing NRD deliberately does not do. NRD's own
 * documentation hands the problem over - "It naturally moves the problem of
 * anti-aliasing to the application side" - and measurement agreed: no NRD dial
 * moved our edge residual by more than a few percent, while having any temporal
 * resolve at all was worth 3.6x. FSR replaces the hand-rolled one.
 *
 * FSR2's Vulkan backend retains its own callback table, including shader
 * reflection and descriptor-layout creation. Its few direct entry points are
 * compiled through volk so the process has exactly one Vulkan loader owner.
 */
class FsrUpscaler : boost::noncopyable {
public:
    FsrUpscaler(graphics::VulkanDevice &device, glm::ivec2 extent);
    ~FsrUpscaler() { deinit(); }

    void init();
    void deinit();
    bool inited() const { return _inited; }

    struct Inputs {
        /** Assembled linear HDR at render resolution, jittered, pre-tonemap. */
        graphics::VulkanImage *color {nullptr};
        /** Post-projection depth in [0,1], jittered like the colour. */
        graphics::VulkanImage *depth {nullptr};
        /** UV-space motion, previous minus current, y down, jitter-free. */
        graphics::VulkanImage *motion {nullptr};
        graphics::VulkanImage *output {nullptr};
    };

    /**
     * @param jitter this frame's sub-pixel offset in pixels, the same value NRD
     *               is given - FSR's convention for it is identical
     * @param frameTimeSeconds converted to the milliseconds FSR expects
     * @param reset true on the first frame and after a camera cut
     */
    void dispatch(VkCommandBuffer cmd, const Inputs &inputs, const glm::vec2 &jitter,
                  float frameTimeSeconds, float cameraNear, float cameraFar, float verticalFov,
                  float sharpness, bool reset);

    /**
     * Sub-pixel offset for a frame index, from FSR's own Halton(2,3) generator.
     * At NativeAA the phase count is 8, which is what our scene graph already
     * used - but taking it from FSR keeps the two from drifting apart.
     */
    static glm::vec2 jitterOffset(int frameIndex, glm::ivec2 extent);

private:
    graphics::VulkanDevice &_device;
    glm::ivec2 _extent;
    bool _inited {false};

    /** The backend scratch arena must outlive the FSR context. */
    std::vector<char> _scratch;
    FfxFsr2Interface _interface {};
    std::unique_ptr<FfxFsr2Context> _context;
    bool _contextCreated {false};
};

} // namespace reone::graphics
