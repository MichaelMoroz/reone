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

#include "reone/graphics/vulkan/gbuffer.h"

#include "reone/graphics/vulkan/device.h"

namespace reone {

namespace graphics {

std::unique_ptr<IGBuffer> makeGBuffer(VulkanDevice &device) {
    return std::make_unique<VulkanGBuffer>(device);
}

std::vector<VkFormat> VulkanGBuffer::nativeColorFormats() {
    // Matching the retained targets: RGBA8 for the colour-ish channels, RGBA8
    // for eye normals (GL uses RGB8, but three-component render targets are not
    // universally supported and the fourth channel costs nothing here), and
    // RG16F for motion, which needs the range and the sign.
    return {
        VK_FORMAT_R8G8B8A8_UNORM, // Diffuse
        VK_FORMAT_R8G8B8A8_UNORM, // EyeNormal
        VK_FORMAT_R8G8B8A8_UNORM, // Lightmap
        VK_FORMAT_R8G8B8A8_UNORM, // SelfIllum
        VK_FORMAT_R16G16_SFLOAT, // Motion
        VK_FORMAT_R16_UINT       // MaterialId
    };
}

std::vector<Format> VulkanGBuffer::colorFormats() const {
    return {Format::R8G8B8A8Unorm, Format::R8G8B8A8Unorm,
            Format::R8G8B8A8Unorm, Format::R8G8B8A8Unorm,
            Format::R16G16Sfloat, Format::R16Uint};
}

void VulkanGBuffer::init(glm::ivec2 extent) {
    _extent = extent;
    auto formats = nativeColorFormats();
    for (int i = 0; i < Count; ++i) {
        _color[i] = std::make_unique<VulkanImage>(_device);
        _color[i]->initColorAttachment(extent, formats[i]);
    }
    _depth = std::make_unique<VulkanImage>(_device);
    _depth->initDepth(extent, nativeDepthFormat());

    // Images are created UNDEFINED and dynamic rendering does not transition
    // them, so the first frame would begin a pass declaring a layout the images
    // are not in. Move them once here; the per-frame cycle takes over after
    // that. The colour attachments deliberately travel in one dependency.
    _device.immediateSubmit([this](VkCommandBuffer cmd) {
        std::vector<VulkanImage *> color;
        color.reserve(Count);
        for (int i = 0; i < Count; ++i) {
            color.push_back(_color[i].get());
        }
        VulkanImage::transitionTo(cmd, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        _depth->transitionTo(cmd, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    });
}

void VulkanGBuffer::deinit() {
    for (auto &image : _color) {
        image.reset();
    }
    _depth.reset();
}

} // namespace graphics

} // namespace reone
