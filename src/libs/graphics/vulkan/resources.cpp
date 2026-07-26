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

#include "reone/graphics/vulkan/resources.h"

#include "reone/graphics/texture.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/system/logutil.h"

namespace reone {

namespace graphics {

bool VulkanResources::supported(PixelFormat format) {
    switch (format) {
    case PixelFormat::R8:
    case PixelFormat::RGB8:
    case PixelFormat::RGBA8:
    case PixelFormat::BGR8:
    case PixelFormat::BGRA8:
    case PixelFormat::DXT1:
    case PixelFormat::DXT5:
        return true;
    default:
        // What remains are render target formats, never uploaded from pixels.
        return false;
    }
}

/**
 * Vulkan has no three-component 8-bit format in practice, so RGB8 and BGR8 are
 * widened to four channels on the way up. The alternative is per-format
 * sampling paths in every shader.
 */
static std::vector<uint8_t> widenToRGBA(const ByteBuffer &pixels,
                                        PixelFormat format,
                                        int width,
                                        int height) {
    size_t texels = static_cast<size_t>(width) * height;
    std::vector<uint8_t> out(texels * 4, 0xff);
    switch (format) {
    case PixelFormat::R8:
        for (size_t i = 0; i < texels; ++i) {
            auto v = static_cast<uint8_t>(pixels[i]);
            out[i * 4 + 0] = v;
            out[i * 4 + 1] = v;
            out[i * 4 + 2] = v;
        }
        break;
    case PixelFormat::RGB8:
        for (size_t i = 0; i < texels; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>(pixels[i * 3 + 0]);
            out[i * 4 + 1] = static_cast<uint8_t>(pixels[i * 3 + 1]);
            out[i * 4 + 2] = static_cast<uint8_t>(pixels[i * 3 + 2]);
        }
        break;
    case PixelFormat::BGR8:
        for (size_t i = 0; i < texels; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>(pixels[i * 3 + 2]);
            out[i * 4 + 1] = static_cast<uint8_t>(pixels[i * 3 + 1]);
            out[i * 4 + 2] = static_cast<uint8_t>(pixels[i * 3 + 0]);
        }
        break;
    case PixelFormat::BGRA8:
        for (size_t i = 0; i < texels; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>(pixels[i * 4 + 2]);
            out[i * 4 + 1] = static_cast<uint8_t>(pixels[i * 4 + 1]);
            out[i * 4 + 2] = static_cast<uint8_t>(pixels[i * 4 + 0]);
            out[i * 4 + 3] = static_cast<uint8_t>(pixels[i * 4 + 3]);
        }
        break;
    case PixelFormat::RGBA8:
        std::memcpy(out.data(), pixels.data(), std::min(out.size(), pixels.size()));
        break;
    default:
        throw std::invalid_argument("Vulkan: unsupported pixel format for upload");
    }
    return out;
}

/**
 * The block-compressed formats, which upload as they stand.
 *
 * The game ships nearly every texture as DXT, so this is the common path rather
 * than a special case. Decompressing on load would cost both time and four to
 * eight times the memory for no benefit: the GPU samples BC directly.
 */
static std::optional<VkFormat> compressedFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::DXT1:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case PixelFormat::DXT5:
        return VK_FORMAT_BC3_UNORM_BLOCK;
    default:
        return std::nullopt;
    }
}

const VulkanImage &VulkanResources::get(const Texture &texture) {
    auto existing = _textures.find(&texture);
    if (existing != _textures.end()) {
        return *existing->second;
    }
    if (!supported(texture.pixelFormat())) {
        throw std::invalid_argument("Vulkan: cannot upload texture " + texture.name() +
                                    " in this pixel format");
    }
    if (texture.layers().empty() || !texture.layers().front().pixels) {
        throw std::invalid_argument("Vulkan: texture " + texture.name() + " has no pixels");
    }
    auto image = std::make_unique<VulkanImage>(_device);
    if (auto compressed = compressedFormat(texture.pixelFormat())) {
        const auto &pixels = *texture.layers().front().pixels;
        image->initSampled2DSized({texture.width(), texture.height()},
                                  *compressed,
                                  pixels.data(),
                                  static_cast<VkDeviceSize>(pixels.size()));
    } else {
        auto widened = widenToRGBA(*texture.layers().front().pixels,
                                   texture.pixelFormat(),
                                   texture.width(),
                                   texture.height());
        image->initSampled2D({texture.width(), texture.height()},
                             VK_FORMAT_R8G8B8A8_UNORM,
                             widened.data());
    }
    debug("Vulkan: uploaded texture " + texture.name(), LogChannel::Graphics);
    return *_textures.insert({&texture, std::move(image)}).first->second;
}

const VulkanMesh &VulkanResources::get(const Mesh &mesh) {
    auto existing = _meshes.find(&mesh);
    if (existing != _meshes.end()) {
        return *existing->second;
    }
    auto uploaded = std::make_unique<VulkanMesh>(_device);
    uploaded->init(mesh);
    return *_meshes.insert({&mesh, std::move(uploaded)}).first->second;
}

void VulkanResources::deinit() {
    _textures.clear();
    _meshes.clear();
}

} // namespace graphics

} // namespace reone
