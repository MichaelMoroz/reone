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

#include "reone/graphics/textureutil.h"

#include "reone/graphics/dxtutil.h"
#include "reone/system/checkutil.h"

namespace reone {

namespace graphics {

static void decompressLayer(int width, int height, Texture::Layer &layer, PixelFormat srcFormat, PixelFormat &dstFormat) {
    if (!isCompressed(srcFormat)) {
        throw std::invalid_argument("format must be either DXT1 or DXT5");
    }

    size_t numPixels = static_cast<size_t>(width) * height;
    const uint8_t *srcPixels = reinterpret_cast<const uint8_t *>(layer.pixels->data());
    std::vector<uint32_t> decompPixels(numPixels);
    uint32_t *decompPixelsPtr = &decompPixels[0];
    bool alpha;

    if (srcFormat == PixelFormat::DXT5) {
        decompressDXT5(width, height, srcPixels, decompPixelsPtr);
        alpha = true;
    } else {
        decompressDXT1(width, height, srcPixels, decompPixelsPtr);
        alpha = false;
    }

    auto destPixels = std::make_shared<ByteBuffer>((alpha ? 4ll : 3ll) * numPixels, '\0');
    uint8_t *destPixelsPtr = reinterpret_cast<uint8_t *>(destPixels->data());
    decompPixelsPtr = &decompPixels[0];

    for (size_t i = 0; i < numPixels; ++i) {
        unsigned long pixel = *(decompPixelsPtr++);
        *(destPixelsPtr++) = (pixel >> 24) & 0xff;
        *(destPixelsPtr++) = (pixel >> 16) & 0xff;
        *(destPixelsPtr++) = (pixel >> 8) & 0xff;
        if (alpha) {
            *(destPixelsPtr++) = pixel & 0xff;
        }
    }

    layer.pixels = std::move(destPixels);
    dstFormat = alpha ? PixelFormat::RGBA8 : PixelFormat::RGB8;
}

static int getBytesPerPixel(PixelFormat format) {
    switch (format) {
    case PixelFormat::R8:
        return 1;
    case PixelFormat::RGB8:
    case PixelFormat::BGR8:
        return 3;
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
        return 4;
    default:
        throw std::invalid_argument("Unsupported pixel format: " + std::to_string(static_cast<int>(format)));
    }
}

void convertArrayTextureToGrid(Texture &texture, int numX, int numY) {
    const int frames = numX * numY;
    if (frames <= 1 || static_cast<int>(texture.layers().size()) != frames) {
        return;
    }
    auto layers = texture.layers();
    auto format = texture.pixelFormat();
    if (isCompressed(format)) {
        PixelFormat decompressed = format;
        for (auto &layer : layers) {
            PixelFormat out;
            decompressLayer(texture.width(), texture.height(), layer, format, out);
            decompressed = out;
        }
        format = decompressed;
    }
    const int bpp = getBytesPerPixel(format);
    const int frameW = texture.width();
    const int frameH = texture.height();
    const int sheetW = frameW * numX;
    const int sheetH = frameH * numY;
    auto sheet = std::make_shared<ByteBuffer>();
    sheet->resize(static_cast<size_t>(sheetW) * sheetH * bpp);
    for (int frame = 0; frame < frames; ++frame) {
        const auto &src = *layers[frame].pixels;
        const int col = frame % numX;
        const int row = frame / numX;
        for (int y = 0; y < frameH; ++y) {
            const size_t srcOff = static_cast<size_t>(y) * frameW * bpp;
            const size_t dstOff =
                (static_cast<size_t>(row * frameH + y) * sheetW + col * frameW) * bpp;
            if (srcOff + static_cast<size_t>(frameW) * bpp > src.size()) {
                return;
            }
            std::memcpy(&(*sheet)[dstOff], &src[srcOff], static_cast<size_t>(frameW) * bpp);
        }
    }
    texture.setType(TextureType::TwoDim);
    texture.setPixelFormat(format);
    std::vector<Texture::Layer> one;
    one.push_back(Texture::Layer {std::move(sheet)});
    texture.setPixels(sheetW, sheetH, format, std::move(one));
}

void convertGridTextureToArray(Texture &texture, int numX, int numY) {
    checkEqual("layers size", static_cast<int>(texture.layers().size()), 1);
    if (isCompressed(texture.pixelFormat())) {
        PixelFormat newFormat;
        decompressLayer(
            texture.width(),
            texture.height(),
            texture.layers().front(),
            texture.pixelFormat(),
            newFormat);
        texture.setPixelFormat(newFormat);
    }
    auto gridPixels = *texture.layers().front().pixels;
    glm::ivec2 frameSize {texture.width() / numX, texture.height() / numY};
    std::vector<Texture::Layer> frameLayers;
    int bytesPerPixel = getBytesPerPixel(texture.pixelFormat());
    size_t framePixelsSize = frameSize.x * frameSize.y * bytesPerPixel;
    for (int i = 0; i < numX * numY; ++i) {
        auto framePixels = std::make_shared<ByteBuffer>();
        framePixels->resize(framePixelsSize);
        for (int x = 0; x < frameSize.x; ++x) {
            for (int y = 0; y < frameSize.y; ++y) {
                int srcRowsToSkip = (i / numX) * frameSize.y + y;
                int srcColsToSkip = (i % numX) * frameSize.x + x;
                int srcPixelIdx = srcRowsToSkip * texture.width() + srcColsToSkip;
                auto srcPixel = &gridPixels[srcPixelIdx * bytesPerPixel];
                int dstPixelIdx = (y * frameSize.x + x);
                auto dstPixel = &(*framePixels)[dstPixelIdx * bytesPerPixel];
                std::memcpy(dstPixel, srcPixel, bytesPerPixel);
            }
        }
        frameLayers.push_back(Texture::Layer {std::move(framePixels)});
    }
    texture.setType(TextureType::TwoDimArray);
    texture.setPixels(
        frameSize.x, frameSize.y,
        texture.pixelFormat(),
        std::move(frameLayers));
}

/**
 * A cycled sheet must not be minified across its own cell boundaries.
 *
 * Every mip level of a grid sheet averages neighbouring cells together, so a
 * minified force field samples a blend of all four frames rather than the one
 * the clock selected - the animation dissolves into a static average as the
 * surface recedes. The cells are small to begin with (128 texels for
 * door_field), so the chain buys nothing here that it does not immediately
 * spend on cross-frame bleed.
 */
void applyCycleFiltering(Texture::Properties &properties, const Texture::Features &features) {
    if (features.procedureType == Texture::ProcedureType::Cycle &&
        (features.numX > 1 || features.numY > 1)) {
        properties.minFilter = Texture::Filtering::Linear;
    }
}

Texture::Properties getTextureProperties(TextureUsage usage) {
    Texture::Properties properties;

    if (usage == TextureUsage::ColorBuffer) {
        properties.minFilter = Texture::Filtering::Linear;
        properties.wrap = Texture::Wrapping::ClampToEdge;

    } else if (usage == TextureUsage::DepthBuffer) {
        properties.minFilter = Texture::Filtering::Nearest;
        properties.magFilter = Texture::Filtering::Nearest;
        properties.wrap = Texture::Wrapping::ClampToBorder;
        properties.borderColor = glm::vec4(1.0f);

    } else if (usage == TextureUsage::EnvironmentMap) {
        properties.wrap = Texture::Wrapping::ClampToEdge;

    } else if (usage == TextureUsage::BumpMap) {

    } else if (usage == TextureUsage::GUI || usage == TextureUsage::Movie) {
        properties.minFilter = Texture::Filtering::Linear;
        properties.wrap = Texture::Wrapping::ClampToEdge;

    } else if (usage == TextureUsage::Font) {
        properties.minFilter = Texture::Filtering::Linear;
        properties.wrap = Texture::Wrapping::ClampToBorder;
        properties.borderColor = glm::vec4(0.0f);
    }

    return properties;
}

} // namespace graphics

} // namespace reone
