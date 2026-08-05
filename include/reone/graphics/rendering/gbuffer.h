/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
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
    MaterialId,
};

inline constexpr std::array<GBufferAttachment, 6> kGBufferAttachments {
    GBufferAttachment::Diffuse,
    GBufferAttachment::EyeNormal,
    GBufferAttachment::Lightmap,
    GBufferAttachment::SelfIllum,
    GBufferAttachment::Motion,
    GBufferAttachment::MaterialId,
};

/** A named, fixed-order set of deferred attachments. */
class GBuffer {
public:
    /** R16_UINT clear value; valid material indices stop at 0xfffe. */
    static constexpr uint32_t kNoMaterial = 0xffffu;

    explicit GBuffer(IRenderer &renderer) :
        _renderer(renderer) {
    }

    GBuffer(const GBuffer &) = delete;
    GBuffer &operator=(const GBuffer &) = delete;

    void init(glm::ivec2 extent);
    void deinit();

    void setSamplers(Sampler color, Sampler depth, Sampler materialId);
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
