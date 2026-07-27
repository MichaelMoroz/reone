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
#include "reone/graphics/vulkan/buffer.h"
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
    case PixelFormat::RG16F:
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
 * Texture pixels use floats as their CPU representation for the 16F formats:
 * OpenGL converts those floats while uploading to its RG16F texture. Vulkan's
 * RG16_SFLOAT image instead expects the IEEE half-float bits in the staging
 * buffer, so preserve the signed noise values while narrowing them here.
 */
static uint16_t floatToHalf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000;
    int exponent = static_cast<int>((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa = (mantissa | 0x800000) >> (1 - exponent);
        return static_cast<uint16_t>(sign | ((mantissa + 0x1000) >> 13));
    }
    if (exponent >= 31) {
        return static_cast<uint16_t>(sign | 0x7c00);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) |
                                 ((mantissa + 0x1000) >> 13));
}

static std::vector<uint8_t> packRG16F(const ByteBuffer &pixels, int width, int height) {
    size_t components = static_cast<size_t>(width) * height * 2;
    std::vector<uint8_t> out(components * sizeof(uint16_t));
    for (size_t i = 0; i < components; ++i) {
        float value;
        std::memcpy(&value, pixels.data() + i * sizeof(value), sizeof(value));
        auto half = floatToHalf(value);
        std::memcpy(out.data() + i * sizeof(half), &half, sizeof(half));
    }
    return out;
}

static std::vector<uint8_t> uploadPixels(const ByteBuffer &pixels,
                                         PixelFormat format,
                                         int width,
                                         int height) {
    if (format == PixelFormat::RG16F) {
        return packRG16F(pixels, width, height);
    }
    return widenToRGBA(pixels, format, width, height);
}

static VkFormat uploadFormat(PixelFormat format) {
    return format == PixelFormat::RG16F ? VK_FORMAT_R16G16_SFLOAT
                                        : VK_FORMAT_R8G8B8A8_UNORM;
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

void VulkanResources::registerExternal(const Texture &texture, const VulkanImage &image) {
    _external[&texture] = &image;
}

void VulkanResources::unregisterExternal(const Texture &texture) {
    _external.erase(&texture);
}

void VulkanResources::invalidate(const Texture &texture) {
    auto existing = _textures.find(&texture);
    if (existing == _textures.end()) {
        return;
    }
    _device.waitIdle();
    _textures.erase(existing);
}

const VulkanImage &VulkanResources::fallbackFor(const Texture &texture,
                                                const std::string &why) {
    if (_warned.insert(why).second) {
        warn("Vulkan: " + why + "; substituting a blank texture", LogChannel::Graphics);
    }
    const uint32_t white = 0xffffffff;
    // Black, not white. A cube map is sampled as incoming radiance, so a white
    // stand-in does not read as "missing" - it reads as a surface lit from
    // every direction at full strength. Black contributes nothing, which is
    // what a texture that is not there should do.
    const uint32_t black = 0xff000000;
    switch (texture.type()) {
    case TextureType::CubeMap:
        if (!_fallbackCube) {
            _fallbackCube = std::make_unique<VulkanImage>(_device);
            _fallbackCube->initSampledLayered({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, 6, true, &black);
        }
        return *_fallbackCube;
    case TextureType::TwoDimArray:
        if (!_fallbackArray) {
            _fallbackArray = std::make_unique<VulkanImage>(_device);
            _fallbackArray->initSampledLayered({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, 1, false, &white);
        }
        return *_fallbackArray;
    default:
        if (!_fallback2D) {
            _fallback2D = std::make_unique<VulkanImage>(_device);
            _fallback2D->initSampled2D({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, &white);
        }
        return *_fallback2D;
    }
}

const VulkanImage &VulkanResources::get(const Texture &texture) {
    auto external = _external.find(&texture);
    if (external != _external.end()) {
        return *external->second;
    }
    auto existing = _textures.find(&texture);
    if (existing != _textures.end()) {
        return *existing->second;
    }
    // Cube map arrays - the irradiance and prefiltered environment maps the GL
    // resolve samples - still have no upload path. Everything else does.
    if (texture.type() == TextureType::CubeMapArray) {
        return fallbackFor(texture, "texture type of " + texture.name() +
                                        " is not uploadable yet");
    }
    if (!supported(texture.pixelFormat())) {
        return fallbackFor(texture, "pixel format of " + texture.name() +
                                        " is not uploadable yet");
    }
    if (texture.layers().empty() || !texture.layers().front().pixels ||
        texture.layers().front().pixels->empty()) {
        return fallbackFor(texture, "texture " + texture.name() + " has no pixels");
    }
    auto image = std::make_unique<VulkanImage>(_device);
    if (texture.type() != TextureType::TwoDim) {
        // A cube map is six faces in the order Vulkan and OpenGL agree on, an
        // array however many frames it has. Either way the layers are already
        // laid out one per Texture::Layer.
        bool cube = texture.type() == TextureType::CubeMap;
        auto compressed = compressedFormat(texture.pixelFormat());
        std::vector<std::vector<uint8_t>> widened;
        std::vector<std::pair<const void *, VkDeviceSize>> layers;
        layers.reserve(texture.layers().size());
        if (!compressed) {
            uint32_t fullMipCount = 1;
            for (int size = std::max(texture.width(), texture.height()); size > 1; size >>= 1) {
                ++fullMipCount;
            }
            widened.reserve(texture.layers().size() * fullMipCount);
        }
        for (const auto &layer : texture.layers()) {
            if (!layer.pixels || layer.pixels->empty()) {
                layers.push_back({nullptr, 0});
                continue;
            }
            if (compressed) {
                layers.push_back({layer.pixels->data(),
                                  static_cast<VkDeviceSize>(layer.pixels->size())});
            } else {
                widened.push_back(uploadPixels(*layer.pixels, texture.pixelFormat(),
                                               texture.width(), texture.height()));
                layers.push_back({widened.back().data(),
                                  static_cast<VkDeviceSize>(widened.back().size())});
            }
        }
        // Exactly six, not at least six: a CUBE view represents six layers, and
        // more than that needs a CUBE_ARRAY view instead.
        if (cube && layers.size() != kNumCubeFaces) {
            return fallbackFor(texture, "cube map " + texture.name() +
                                            " does not have exactly six faces");
        }
        // A layer with no pixels is left out of the copy, and an image region
        // that was never written keeps whatever the allocation happened to
        // hold. Fill it rather than sampling undefined memory.
        //
        // The size comes from a sibling layer rather than from the extent: for
        // a block-compressed format there is no per-texel size, so width times
        // height times four would be neither the right length nor decodable.
        VkDeviceSize layerSize = 0;
        for (const auto &layer : layers) {
            if (layer.second > 0) {
                layerSize = layer.second;
                break;
            }
        }
        if (layerSize == 0) {
            return fallbackFor(texture, "texture " + texture.name() + " has no pixels");
        }
        std::vector<uint8_t> blank;
        for (auto &layer : layers) {
            if (layer.first) {
                continue;
            }
            if (blank.empty()) {
                blank.assign(static_cast<size_t>(layerSize), 0);
            }
            layer = {blank.data(), layerSize};
        }
        // GL generates a chain only when the first face did not ship one. TPC
        // cubemaps normally carry an identical chain for every face; upload
        // that data verbatim, including the per-face/per-level byte sizes.
        uint32_t authoredMips = 0;
        while (authoredMips < texture.layers().front().mips.size()) {
            const auto &mip = texture.layers().front().mips[authoredMips];
            if (!mip || mip->empty()) {
                break;
            }
            ++authoredMips;
        }
        if (authoredMips > 0) {
            for (const auto &layer : texture.layers()) {
                if (layer.mips.size() < authoredMips) {
                    return fallbackFor(texture, "texture " + texture.name() +
                                                    " has an incomplete layered mip chain");
                }
                for (uint32_t mip = 0; mip < authoredMips; ++mip) {
                    if (!layer.mips[mip] || layer.mips[mip]->empty()) {
                        return fallbackFor(texture, "texture " + texture.name() +
                                                        " has an incomplete layered mip chain");
                    }
                }
            }
        }

        uint32_t fullMipCount = 1;
        for (int size = std::max(texture.width(), texture.height()); size > 1; size >>= 1) {
            ++fullMipCount;
        }
        // Vulkan cannot blit BC images. Those must use the authored levels,
        // just as the 2D path does; uncompressed cubes/arrays without a chain
        // take the same generated-mip branch as GL.
        bool generateMips = authoredMips == 0 && !compressed && fullMipCount > 1;
        uint32_t mipCount = authoredMips > 0 ? authoredMips + 1
                                             : (generateMips ? fullMipCount : 1);
        std::vector<VulkanImage::Subresource> subresources;
        subresources.reserve(layers.size() * (authoredMips + 1));
        for (uint32_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
            subresources.push_back({layers[layerIndex].first, layers[layerIndex].second, layerIndex, 0});
            for (uint32_t mip = 0; mip < authoredMips; ++mip) {
                const auto &level = texture.layers()[layerIndex].mips[mip];
                if (compressed) {
                    subresources.push_back({level->data(), static_cast<VkDeviceSize>(level->size()),
                                             layerIndex, mip + 1});
                } else {
                    widened.push_back(uploadPixels(*level, texture.pixelFormat(),
                                                   std::max(1, texture.width() >> (mip + 1)),
                                                   std::max(1, texture.height() >> (mip + 1))));
                    subresources.push_back({widened.back().data(),
                                             static_cast<VkDeviceSize>(widened.back().size()),
                                             layerIndex, mip + 1});
                }
            }
        }
        image->initSampledChain({texture.width(), texture.height()},
                                compressed ? *compressed : uploadFormat(texture.pixelFormat()),
                                cube, static_cast<uint32_t>(layers.size()), mipCount,
                                subresources, generateMips);
        auto chain = generateMips ? "generated" : (authoredMips ? "authored" : "base-level");
        debug("Vulkan: uploaded " + std::string(chain) + " mip chain for " + texture.name(),
              LogChannel::Graphics);
        image->setSampler(_samplers.get(texture.properties()));
        debug("Vulkan: uploaded texture " + texture.name(), LogChannel::Graphics);
        return *_textures.insert({&texture, std::move(image)}).first->second;
    }
    const auto &layer = texture.layers().front();
    std::vector<VulkanImage::Subresource> subresources;
    // Kept alive until the upload has copied out of them.
    std::vector<std::vector<uint8_t>> widened;
    if (auto compressed = compressedFormat(texture.pixelFormat())) {
        subresources.push_back({layer.pixels->data(),
                                static_cast<VkDeviceSize>(layer.pixels->size()), 0, 0});
        uint32_t mip = 1;
        for (const auto &level : layer.mips) {
            // Stop at the first gap rather than skipping it: the chain has to
            // be contiguous, or the level count would claim levels that were
            // never written and sampling one would read whatever was there.
            if (!level || level->empty()) {
                break;
            }
            subresources.push_back({level->data(),
                                    static_cast<VkDeviceSize>(level->size()), 0, mip});
            ++mip;
        }
        image->initSampledChain({texture.width(), texture.height()}, *compressed,
                                false, 1, static_cast<uint32_t>(subresources.size()),
                                subresources);
    } else {
        uint32_t authoredMips = 0;
        while (authoredMips < layer.mips.size()) {
            const auto &level = layer.mips[authoredMips];
            if (!level || level->empty()) {
                break;
            }
            ++authoredMips;
        }
        uint32_t fullMipCount = 1;
        for (int size = std::max(texture.width(), texture.height()); size > 1; size >>= 1) {
            ++fullMipCount;
        }
        // This is the 2D counterpart of the cube/layered path above. OpenGL
        // calls glGenerateMipmap for an uncompressed texture that did not ship
        // a chain, including ordinary 2D environment maps.
        // Leaving Vulkan at level zero makes every explicit source LOD in the
        // IBL prefilter clamp to that sharp base image instead.
        bool generateMips = authoredMips == 0 && fullMipCount > 1;
        uint32_t mipCount = authoredMips > 0 ? authoredMips + 1
                                             : (generateMips ? fullMipCount : 1);
        widened.reserve(mipCount);
        widened.push_back(uploadPixels(*layer.pixels, texture.pixelFormat(),
                                       texture.width(), texture.height()));
        subresources.push_back({widened.back().data(),
                                static_cast<VkDeviceSize>(widened.back().size()), 0, 0});
        for (uint32_t mip = 1; mip <= authoredMips; ++mip) {
            const auto &level = layer.mips[mip - 1];
            widened.push_back(uploadPixels(*level, texture.pixelFormat(),
                                           std::max(1, texture.width() >> mip),
                                           std::max(1, texture.height() >> mip)));
            subresources.push_back({widened.back().data(),
                                    static_cast<VkDeviceSize>(widened.back().size()),
                                    0, mip});
        }
        image->initSampledChain({texture.width(), texture.height()},
                                uploadFormat(texture.pixelFormat()),
                                false, 1, mipCount, subresources, generateMips);
        auto chain = generateMips ? "generated" : (authoredMips ? "authored" : "base-level");
        debug("Vulkan: uploaded " + std::string(chain) + " mip chain for " + texture.name(),
              LogChannel::Graphics);
    }
    image->setSampler(_samplers.get(texture.properties()));
    debug("Vulkan: uploaded texture " + texture.name(), LogChannel::Graphics);
    return *_textures.insert({&texture, std::move(image)}).first->second;
}

VkBuffer VulkanResources::zeroBuffer() {
    if (!_zeroBuffer) {
        // Large enough for the widest attribute any shader declares.
        std::array<float, 4> zeros {};
        _zeroBuffer = std::make_unique<VulkanBuffer>(_device);
        _zeroBuffer->initDeviceLocal(sizeof(zeros), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     zeros.data());
    }
    return _zeroBuffer->handle();
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

void VulkanResources::clearUploaded() {
    _textures.clear();
    _meshes.clear();
}

void VulkanResources::deinit() {
    _zeroBuffer.reset();
    _fallback2D.reset();
    _fallbackArray.reset();
    _fallbackCube.reset();
    _external.clear();
    _textures.clear();
    _meshes.clear();
    // Explicitly, not from the member destructor: this object outlives
    // VulkanDevice::deinit, and destroying a sampler after the device is gone
    // is a use-after-free.
    _samplers.deinit();
}

} // namespace graphics

} // namespace reone
