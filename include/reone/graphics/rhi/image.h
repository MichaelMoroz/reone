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

#include <vector>

#include "rhi.h"

namespace reone {

namespace graphics {

/** Image operations used by the 2D and image-based-lighting clients. */
class IImage {
public:
    virtual ~IImage() = default;

    virtual void initColorAttachment(glm::ivec2 extent, Format format) = 0;
    /** A single depth target written by geometry and sampled by later passes. */
    virtual void initDepthAttachment(glm::ivec2 extent, Format format) = 0;
    /** A layered depth target used by one shadow kind. */
    virtual void initLayeredDepthAttachment(glm::ivec2 extent, Format format,
                                            int layers, bool cube) = 0;
    virtual void initCubeArrayAttachment(glm::ivec2 faceExtent, Format format,
                                         int cubes, int mips) = 0;
    virtual ImageView attachmentView(int cube, int mip) = 0;
    /** One cube face used as a color attachment in a per-face pass. */
    virtual ImageView faceAttachmentView(int cube, int face, int mip = 0) = 0;
    virtual void setSampler(Sampler sampler) = 0;
    virtual void deinit() = 0;

    virtual ImageView sampleView() const = 0;
    virtual Sampler sampleSampler() const = 0;
    virtual Format pixelFormat() const = 0;
    virtual glm::ivec2 extent() const = 0;
    virtual int mipLevels() const = 0;
    virtual std::vector<uint8_t> readBack(bool depth) const = 0;
    virtual std::vector<uint8_t> readBack(uint32_t mip, uint32_t layers) const = 0;
};

} // namespace graphics

} // namespace reone
