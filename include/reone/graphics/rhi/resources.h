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

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "image.h"
#include "../texture.h"

namespace reone::graphics {

/** Device-side resource lookup used while recording backend-free draws. */
class IResources {
public:
    using IndexedImage = std::pair<uint32_t, const IImage *>;

    virtual ~IResources() = default;

    virtual void deinit() = 0;
    /** Create an image owned by the caller rather than by this resource cache. */
    virtual std::unique_ptr<IImage> makeImage() = 0;
    virtual const IImage &get(const Texture &texture) = 0;
    /** Return the stable bindless id assigned during texture upload, if any. */
    virtual std::optional<uint32_t> textureId(const Texture &texture) = 0;
    virtual Sampler sampler(const Texture::Properties &properties) = 0;
    virtual std::vector<IndexedImage> uploadedTextures() const = 0;
    virtual std::vector<IndexedImage> uploadedTextureArrays() const = 0;
    virtual std::vector<IndexedImage> uploadedTextureCubes() const = 0;
    virtual void registerExternal(const Texture &texture, const IImage &image) = 0;
    virtual void unregisterExternal(const Texture &texture) = 0;
    virtual bool isExternal(const Texture &texture) const = 0;
};

} // namespace reone::graphics
