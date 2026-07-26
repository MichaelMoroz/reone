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

/**
 * The opaque geometry pass targets: what a fragment knows about the surface it
 * covered, kept so lighting can be resolved once per pixel afterwards rather
 * than once per fragment.
 *
 * The attachments, their formats and their order match the OpenGL PBR pipeline
 * exactly, so the same fragment shader output struct serves both and the two
 * can be compared frame by frame.
 */
class VulkanGBuffer : boost::noncopyable {
public:
    /** Attachment order, matching fbOpaqueGeometry in the GL pipeline. */
    enum Attachment {
        Diffuse = 0,
        EyeNormal,
        Lightmap,
        SelfIllum,
        Motion,
        Count
    };

    VulkanGBuffer(VulkanDevice &device) :
        _device(device) {
    }

    void init(glm::ivec2 extent);
    void deinit();

    /** Formats in attachment order, for building a pipeline against this set. */
    static std::vector<VkFormat> colorFormats();
    static VkFormat depthFormat() { return VK_FORMAT_D32_SFLOAT; }

    glm::ivec2 extent() const { return _extent; }
    const VulkanImage &color(int attachment) const { return *_color[attachment]; }
    const VulkanImage &depth() const { return *_depth; }

    /**
     * Move every colour attachment between layouts in one barrier. The pass
     * writes them as attachments and the resolve reads them as textures, so
     * they change layout twice a frame together.
     */
    void transitionColor(VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to) const;

private:
    VulkanDevice &_device;

    glm::ivec2 _extent {0};
    std::array<std::unique_ptr<VulkanImage>, Count> _color;
    std::unique_ptr<VulkanImage> _depth;
};

} // namespace graphics

} // namespace reone
