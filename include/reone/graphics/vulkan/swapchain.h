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

#include <VkBootstrap.h>

namespace reone {

namespace graphics {

class VulkanDevice;

/**
 * The images the window is presented from, and the views onto them.
 *
 * Separate from VulkanDevice because it is the part that gets thrown away and
 * rebuilt - on resize, and whenever a present reports the surface no longer
 * matches the window.
 */
class VulkanSwapchain : boost::noncopyable {
public:
    VulkanSwapchain(VulkanDevice &device) :
        _device(device) {
    }

    ~VulkanSwapchain() { deinit(); }

    void init(glm::ivec2 extent, bool vsync);
    void deinit();

    /** Tear down and rebuild at a new size, keeping the same format. */
    void recreate(glm::ivec2 extent);
    void setVsync(bool enabled) { _vsync = enabled; }

    VkSwapchainKHR handle() const { return _swapchain.swapchain; }
    VkFormat imageFormat() const { return _swapchain.image_format; }
    glm::ivec2 extent() const { return _extent; }
    uint32_t imageCount() const { return static_cast<uint32_t>(_images.size()); }

    VkImage image(uint32_t index) const { return _images[index]; }
    VkImageView imageView(uint32_t index) const { return _imageViews[index]; }

private:
    VulkanDevice &_device;

    bool _inited {false};
    bool _vsync {true};

    vkb::Swapchain _swapchain;
    std::vector<VkImage> _images;
    std::vector<VkImageView> _imageViews;
    glm::ivec2 _extent {0};

    void build(glm::ivec2 extent);
    void destroy();
};

} // namespace graphics

} // namespace reone
