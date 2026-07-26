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

#include "image.h"

namespace reone {

namespace graphics {

class VulkanDevice;
class VulkanUniformRing;

/**
 * The uniform descriptor set: one dynamic uniform buffer per block, at the
 * bindings `slang/uniforms.slang` pins and `UniformBlockBindingPoints` names.
 *
 * Dynamic, so a draw selects its slice of the frame's arena with an offset
 * given at bind time rather than by owning a descriptor. Without that, every
 * draw would need its own descriptor set and the set count would track the
 * draw count.
 *
 * One set per frame in flight is enough because the descriptors themselves
 * never change - they always point at that frame's whole arena, and only the
 * offsets move.
 */
class VulkanDescriptors : boost::noncopyable {
public:
    /** Must match the number of blocks in uniforms.h and uniforms.slang. */
    static constexpr int kNumUniformBlocks = 10;

    /** Must cover every unit in TextureUnits. */
    static constexpr int kNumTextures = 21;

    /**
     * Uniform blocks and textures both start numbering at zero, so they cannot
     * share a set. Uniforms are set 0 and textures set 1, which the shaders
     * spell as [[vk::binding(n, 1)]].
     */
    static constexpr int kUniformSet = 0;
    static constexpr int kTextureSet = 1;

    VulkanDescriptors(VulkanDevice &device) :
        _device(device) {
    }

    ~VulkanDescriptors();

    void init(int framesInFlight, VulkanUniformRing &ring);
    void deinit();

    VkDescriptorSetLayout uniformLayout() const { return _uniformLayout; }
    VkDescriptorSet uniformSet(int frame) const { return _uniformSets[frame]; }

    VkDescriptorSetLayout textureLayout() const { return _textureLayout; }
    VkDescriptorSet textureSet() const { return _textureSet; }

    /**
     * Point a texture unit at @p image. Every unit starts on a 1x1 white
     * default, because a set may not be bound with any descriptor left
     * unwritten and a shader is free to sample a unit no material filled in.
     */
    void setTexture(int unit, const VulkanImage &image);

private:
    VulkanDevice &_device;

    VkDescriptorPool _pool {VK_NULL_HANDLE};
    VkDescriptorSetLayout _uniformLayout {VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> _uniformSets;

    VkDescriptorSetLayout _textureLayout {VK_NULL_HANDLE};
    VkDescriptorSet _textureSet {VK_NULL_HANDLE};
    VkSampler _sampler {VK_NULL_HANDLE};
    std::unique_ptr<VulkanImage> _defaultTexture;
};

} // namespace graphics

} // namespace reone
