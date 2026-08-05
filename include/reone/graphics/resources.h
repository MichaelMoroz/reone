/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "image.h"
#include "texture.h"

namespace reone::graphics {

/** Device-side resource lookup used while recording backend-free draws. */
class IResources {
public:
    virtual ~IResources() = default;

    virtual void deinit() = 0;
    virtual const IImage &get(const Texture &texture) = 0;
    virtual Sampler sampler(const Texture::Properties &properties) = 0;
    virtual bool isExternal(const Texture &texture) const = 0;
};

} // namespace reone::graphics
