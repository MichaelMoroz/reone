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

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace reone::graphics {

class ICommandBuffer;
class IImage;

/**
 * The images one upscale reads and writes.
 *
 * Layouts are part of the contract because the backend transitions them from
 * the state it is told they are in: the three inputs must be in ShaderRead and
 * the output in General when the dispatch is recorded, and the backend leaves
 * them exactly there.
 *
 * Depth and motion come from the G-buffer in every mode. Primary visibility is
 * rasterized even when the picture is traced, so the same two attachments
 * describe the same surfaces the colour was shaded for.
 */
struct UpscalerInputs {
    IImage *color {nullptr};
    IImage *depth {nullptr};
    IImage *motion {nullptr};
    IImage *output {nullptr};
    /**
     * Multiplies a sampled motion vector into pixels, in the upscaler's own
     * convention. Separate from the images because the two motion encodings in
     * this engine differ by a sign per axis; see the dispatch site.
     */
    glm::vec2 motionScale {1.0f};

    /**
     * The extra guides DLSS Ray Reconstruction reads, and FSR ignores.
     *
     * Optional rather than a second interface: RR occupies the same slot, at
     * the same point in the frame, differing only in how much of the frame it
     * is told about. Null everywhere but the traced and PBR channel providers,
     * which is exactly where RR can run.
     *
     * The two "albedo" guides are this engine's demodulation factors, not
     * textbook albedos - which is the self-consistent choice, since the colour
     * handed over was modulated by these same two.
     */
    IImage *diffuseAlbedo {nullptr};
    IImage *specularAlbedo {nullptr};
    /** World normal in xyz, linear roughness in w. */
    IImage *normalRoughness {nullptr};

    /**
     * Unjittered, and previous-frame counterparts, for the resolver that needs
     * to build a clip-to-previous-clip transform of its own. FSR derives what
     * it needs from the scalars beside it and reads none of these.
     */
    glm::mat4 view {1.0f};
    glm::mat4 projection {1.0f};
    glm::mat4 prevView {1.0f};
    glm::mat4 prevProjection {1.0f};
    glm::vec3 cameraPosition {0.0f};
};

/** A temporal resolve that reprojects the previous frame onto this one. */
class IUpscaler {
public:
    virtual ~IUpscaler() = default;

    virtual void dispatch(ICommandBuffer &commandBuffer, const UpscalerInputs &inputs,
                          const glm::vec2 &jitter, float frameTimeSeconds,
                          float cameraNear, float cameraFar, float verticalFov,
                          float sharpness, bool reset) = 0;
};

} // namespace reone::graphics
