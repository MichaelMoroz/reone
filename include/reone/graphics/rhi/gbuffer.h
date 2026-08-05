/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "image.h"

namespace reone::graphics {

/** The retained surface fields written together by opaque geometry. */
enum class GBufferAttachment {
    Diffuse,
    EyeNormal,
    Lightmap,
    SelfIllum,
    Motion,
    MaterialId,
    Count,
};

/** A named, fixed-order set of deferred attachments. */
class IGBuffer {
public:
    virtual ~IGBuffer() = default;

    static constexpr uint32_t kNoMaterial = 0xffffu;

    virtual void init(glm::ivec2 extent) = 0;
    virtual void deinit() = 0;
    virtual void setSamplers(Sampler color, Sampler depth, Sampler materialId) = 0;
    virtual IImage &color(GBufferAttachment attachment) = 0;
    virtual IImage &depth() = 0;
    virtual std::vector<Format> colorFormats() const = 0;
    virtual Format depthFormat() const = 0;
    virtual glm::ivec2 extent() const = 0;
};

} // namespace reone::graphics
