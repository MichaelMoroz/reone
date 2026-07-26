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

#pragma once

#include <volk.h>

#include "reone/graphics/texture.h"

namespace reone {

namespace graphics {

class VulkanDevice;

/**
 * Samplers, one per distinct set of filtering settings.
 *
 * In OpenGL, filtering and wrapping belong to the texture object, so every
 * texture carries its own. Vulkan splits them out into a sampler, and reone
 * had exactly one - trilinear, repeat, no anisotropy - written into every
 * descriptor. That is right for material textures and wrong for everything
 * else: render targets and GUI images want no mip filtering and clamping,
 * depth wants nearest and a border, fonts want a border too.
 *
 * There are only a handful of distinct combinations, all of them decided by
 * TextureUsage, so they are cached rather than created per texture.
 */
class VulkanSamplers : boost::noncopyable {
public:
    VulkanSamplers(VulkanDevice &device) :
        _device(device) {
    }

    ~VulkanSamplers() { deinit(); }

    void deinit();

    /** A sampler matching @p properties, created on first use. */
    VkSampler get(const Texture::Properties &properties);

    /** The trilinear repeating sampler that stands in where none is known. */
    VkSampler defaultSampler() { return get(Texture::Properties()); }

private:
    struct Key {
        Texture::Filtering minFilter;
        Texture::Filtering magFilter;
        Texture::Wrapping wrap;
        glm::vec4 borderColor;
        float anisotropy;

        bool operator==(const Key &other) const {
            return minFilter == other.minFilter &&
                   magFilter == other.magFilter &&
                   wrap == other.wrap &&
                   borderColor == other.borderColor &&
                   anisotropy == other.anisotropy;
        }
    };

    struct KeyHash {
        size_t operator()(const Key &key) const;
    };

    VulkanDevice &_device;
    std::unordered_map<Key, VkSampler, KeyHash> _samplers;
};

} // namespace graphics

} // namespace reone
