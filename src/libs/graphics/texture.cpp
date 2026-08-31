/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include "reone/graphics/texture.h"

namespace reone {
namespace graphics {

void Texture::init() {
}

void Texture::clear(int w, int h, PixelFormat format, int numLayers) {
    _width = w;
    _height = h;
    _pixelFormat = format;
    _layers.clear();
    _layers.resize(numLayers);
}

void Texture::setPixels(int w, int h, PixelFormat format, Layer layer) {
    setPixels(w, h, format, std::vector<Layer> {std::move(layer)});
}

void Texture::setPixels(int w, int h, PixelFormat format, std::vector<Layer> layers) {
    if (layers.empty()) {
        throw std::invalid_argument("layers is empty");
    }
    _width = w;
    _height = h;
    _pixelFormat = format;
    _layers = std::move(layers);
}

} // namespace graphics
} // namespace reone
