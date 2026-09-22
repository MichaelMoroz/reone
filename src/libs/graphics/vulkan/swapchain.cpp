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

#include "reone/graphics/vulkan/swapchain.h"

#include "reone/graphics/vulkan/device.h"
#include "reone/system/logutil.h"

namespace reone {

namespace graphics {

void VulkanSwapchain::init(glm::ivec2 extent, bool vsync) {
    if (_inited) {
        return;
    }
    _vsync = vsync;
    build(extent);
    _inited = true;
}

void VulkanSwapchain::deinit() {
    if (!_inited) {
        return;
    }
    destroy();
    _inited = false;
}

void VulkanSwapchain::recreate(glm::ivec2 extent) {
    // Everything referencing the old images must have finished first, and at
    // this point there is nothing in flight worth preserving, so the blunt
    // instrument is the correct one.
    vkDeviceWaitIdle(_device.handle());
    destroy();
    build(extent);
}

void VulkanSwapchain::build(glm::ivec2 extent) {
    // FIFO is the only present mode required to exist, so it is what vsync
    // means here. Mailbox is the usual choice for uncapped, falling back to
    // immediate and then to FIFO again if neither is offered.
    auto builder = vkb::SwapchainBuilder(_device.bootstrapDevice())
                       .set_desired_extent(extent.x, extent.y)
                       .set_desired_format(VkSurfaceFormatKHR {
                           VK_FORMAT_B8G8R8A8_UNORM,
                           VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                       // SRC so a finished frame can be read back for the
                       // screenshot harness, DST so it can be cleared or blitted
                       // into outside a render pass.
                       .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                              VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (_vsync) {
        builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
    } else {
        builder.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR);
    }

    auto result = builder.build();
    if (!result) {
        throw std::runtime_error("Vulkan: swapchain creation failed: " +
                                 result.error().message());
    }
    _swapchain = result.value();
    _images = _swapchain.get_images().value();
    _imageViews = _swapchain.get_image_views().value();
    _extent = {_swapchain.extent.width, _swapchain.extent.height};

    info(str(boost::format("Vulkan swapchain: %dx%d, %d images") %
             _extent.x % _extent.y % _images.size()));
}

void VulkanSwapchain::destroy() {
    _swapchain.destroy_image_views(_imageViews);
    _imageViews.clear();
    _images.clear();
    vkb::destroy_swapchain(_swapchain);
}

} // namespace graphics

} // namespace reone
