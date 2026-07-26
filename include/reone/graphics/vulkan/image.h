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

#include <vk_mem_alloc.h>

namespace reone {

namespace graphics {

class VulkanDevice;

/**
 * A device-local image, its allocation and its view, freed together.
 *
 * Unlike a GL texture, an image has a layout: the same memory is arranged
 * differently for being written by a transfer, sampled by a shader, or used as
 * an attachment, and moving between those is an explicit barrier. Upload is
 * therefore three steps rather than one - transition to a transfer target, copy
 * from a staging buffer, transition to something a shader can read.
 */
class VulkanImage : boost::noncopyable {
public:
    VulkanImage(VulkanDevice &device) :
        _device(device) {
    }

    ~VulkanImage() { deinit(); }

    /**
     * Create a sampled 2D image and fill it from @p data, which must be
     * @p extent.x * @p extent.y texels in @p format. Leaves the image in
     * SHADER_READ_ONLY_OPTIMAL.
     */
    void initSampled2D(glm::ivec2 extent, VkFormat format, const void *data);

    /**
     * A depth attachment. Left in UNDEFINED: dynamic rendering transitions it
     * on first use, and its contents never need to survive a frame.
     */
    void initDepth(glm::ivec2 extent, VkFormat format);

    /**
     * A colour attachment that is also sampled afterwards, which is what every
     * G-buffer target is.
     */
    void initColorAttachment(glm::ivec2 extent, VkFormat format);

    void deinit();

    VkImage handle() const { return _image; }
    VkImageView view() const { return _view; }
    glm::ivec2 extent() const { return _extent; }
    VkFormat format() const { return _format; }

private:
    VulkanDevice &_device;

    VkImage _image {VK_NULL_HANDLE};
    VkImageView _view {VK_NULL_HANDLE};
    VmaAllocation _allocation {VK_NULL_HANDLE};
    glm::ivec2 _extent {0};
    VkFormat _format {VK_FORMAT_UNDEFINED};
};

} // namespace graphics

} // namespace reone
