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

#include <array>
#include <memory>
#include <vector>

#include "reone/graphics/rhi/image.h"
#include "reone/graphics/rhi/renderer.h"

namespace reone::graphics {

/** The retained surface fields written together by opaque geometry. */
enum class GBufferAttachment {
    Diffuse,
    EyeNormal,
    Lightmap,
    SelfIllum,
    Motion,
    /**
     * The merged-geometry triangle the fragment came from.
     *
     * The one field a consumer can expand back into geometry rather than
     * decode: the triangle's own vertices give the GEOMETRIC normal exactly,
     * which an interpolated, normal-mapped stored normal cannot, and
     * mergedMaterialIds indexed by it gives the material - so this attachment
     * replaced the material id rather than joining it.
     */
    TriangleId,
};

inline constexpr std::array<GBufferAttachment, 6> kGBufferAttachments {
    GBufferAttachment::Diffuse,
    GBufferAttachment::EyeNormal,
    GBufferAttachment::Lightmap,
    GBufferAttachment::SelfIllum,
    GBufferAttachment::Motion,
    GBufferAttachment::TriangleId,
};

/** A named, fixed-order set of deferred attachments. */
class GBuffer {
public:
    /**
     * R32_UINT clear value: no geometry covered this pixel.
     *
     * Every consumer's sky test. Merged triangle ids are dense from zero, so
     * the sentinel is the top of the range and a scene would have to reach
     * 4294967295 triangles to collide with it; prepareMergedScene refuses
     * before that can happen.
     */
    static constexpr uint32_t kNoTriangle = 0xffffffffu;

    explicit GBuffer(IRenderer &renderer) :
        _renderer(renderer) {
    }

    GBuffer(const GBuffer &) = delete;
    GBuffer &operator=(const GBuffer &) = delete;

    void init(glm::ivec2 extent);
    void deinit();

    void setSamplers(Sampler color, Sampler depth, Sampler triangleId);
    IImage &color(GBufferAttachment attachment);
    IImage &depth() { return *_depth; }
    std::vector<IImage *> colorImages();
    std::vector<Format> colorFormats() const;
    Format depthFormat() const { return Format::D32Sfloat; }
    glm::ivec2 extent() const { return _extent; }

private:
    static size_t attachmentIndex(GBufferAttachment attachment);

    IRenderer &_renderer;
    glm::ivec2 _extent {0};
    std::array<std::unique_ptr<IImage>, kGBufferAttachments.size()> _color;
    std::unique_ptr<IImage> _depth;
};

} // namespace reone::graphics
