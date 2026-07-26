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

#include "reone/graphics/vulkan/samplers.h"

#include "reone/graphics/vulkan/device.h"

namespace reone {

namespace graphics {

size_t VulkanSamplers::KeyHash::operator()(const Key &key) const {
    size_t h = std::hash<int>()(static_cast<int>(key.minFilter));
    auto mix = [&h](size_t v) { h = h * 31 + v; };
    mix(std::hash<int>()(static_cast<int>(key.magFilter)));
    mix(std::hash<int>()(static_cast<int>(key.wrap)));
    mix(std::hash<float>()(key.anisotropy));
    for (int i = 0; i < 4; ++i) {
        mix(std::hash<float>()(key.borderColor[i]));
    }
    return h;
}

/** Whether a minification filter walks the mip chain at all. */
static bool isMipmapped(Texture::Filtering filter) {
    switch (filter) {
    case Texture::Filtering::NearestMipmapNearest:
    case Texture::Filtering::LinearMipmapNearest:
    case Texture::Filtering::NearestMipmapLinear:
    case Texture::Filtering::LinearMipmapLinear:
        return true;
    default:
        return false;
    }
}

/** The within-level filter: what OpenGL calls the first half of the name. */
static VkFilter texelFilter(Texture::Filtering filter) {
    switch (filter) {
    case Texture::Filtering::Nearest:
    case Texture::Filtering::NearestMipmapNearest:
    case Texture::Filtering::NearestMipmapLinear:
        return VK_FILTER_NEAREST;
    default:
        return VK_FILTER_LINEAR;
    }
}

/** The between-levels filter: the second half. */
static VkSamplerMipmapMode mipmapMode(Texture::Filtering filter) {
    switch (filter) {
    case Texture::Filtering::NearestMipmapLinear:
    case Texture::Filtering::LinearMipmapLinear:
        return VK_SAMPLER_MIPMAP_MODE_LINEAR;
    default:
        return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
}

static VkSamplerAddressMode addressMode(Texture::Wrapping wrap) {
    switch (wrap) {
    case Texture::Wrapping::ClampToEdge:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case Texture::Wrapping::ClampToBorder:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

/**
 * Vulkan has no arbitrary border colour without an extension, only four fixed
 * ones, so the requested colour is snapped to the nearest of them. Everything
 * reone actually asks for is either transparent black or opaque white.
 */
static VkBorderColor borderColor(const glm::vec4 &color) {
    if (color.a < 0.5f) {
        return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    float luma = (color.r + color.g + color.b) / 3.0f;
    return luma >= 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                        : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
}

VkSampler VulkanSamplers::get(const Texture::Properties &properties) {
    Key key {properties.minFilter, properties.magFilter, properties.wrap,
             properties.borderColor, properties.anisotropy};
    auto existing = _samplers.find(key);
    if (existing != _samplers.end()) {
        return existing->second;
    }

    VkSamplerCreateInfo info {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.minFilter = texelFilter(properties.minFilter);
    info.magFilter = texelFilter(properties.magFilter);
    info.mipmapMode = mipmapMode(properties.minFilter);
    info.addressModeU = addressMode(properties.wrap);
    info.addressModeV = info.addressModeU;
    info.addressModeW = info.addressModeU;
    info.borderColor = borderColor(properties.borderColor);
    // A non-mipmapped filter has to be clamped to the base level. Leaving the
    // range open would let a render target or a GUI image minify into levels
    // that were never uploaded.
    info.maxLod = isMipmapped(properties.minFilter) ? VK_LOD_CLAMP_NONE : 0.0f;

    // Anisotropy only means anything alongside mip filtering, and the device
    // has its own ceiling regardless of what the configuration asked for.
    float anisotropy = std::min(properties.anisotropy, _device.maxAnisotropy());
    if (anisotropy > 1.0f && isMipmapped(properties.minFilter)) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = anisotropy;
    }

    VkSampler sampler {VK_NULL_HANDLE};
    if (vkCreateSampler(_device.handle(), &info, nullptr, &sampler) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: sampler creation failed");
    }
    _samplers.insert({key, sampler});
    return sampler;
}

void VulkanSamplers::deinit() {
    for (auto &entry : _samplers) {
        vkDestroySampler(_device.handle(), entry.second, nullptr);
    }
    _samplers.clear();
}

} // namespace graphics

} // namespace reone
