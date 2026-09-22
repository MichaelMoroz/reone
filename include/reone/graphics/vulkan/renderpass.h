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

#include <array>
#include <initializer_list>
#include <optional>

namespace reone {

namespace graphics {

/** One attachment in a dynamic-rendering scope. */
struct RenderPassAttachment {
    VkImageView imageView {VK_NULL_HANDLE};
    VkImageLayout imageLayout {VK_IMAGE_LAYOUT_UNDEFINED};
    VkAttachmentLoadOp loadOp {VK_ATTACHMENT_LOAD_OP_DONT_CARE};
    VkAttachmentStoreOp storeOp {VK_ATTACHMENT_STORE_OP_DONT_CARE};
    VkClearValue clearValue {};
};

/**
 * Records a complete dynamic-rendering scope.
 *
 * Dynamic rendering does not infer attachment state from a framebuffer: every
 * attachment must be named in the order the pipeline was built against. The
 * initializer list deliberately makes a single attachment one element, rather
 * than treating it as a value to broadcast across the pass.
 */
class RenderPassScope {
public:
    RenderPassScope(VkCommandBuffer commandBuffer,
                    glm::ivec2 extent,
                    std::initializer_list<RenderPassAttachment> colorAttachments,
                    std::optional<RenderPassAttachment> depthAttachment = std::nullopt,
                    uint32_t viewMask = 0,
                    bool invertedViewport = false);
    ~RenderPassScope();

    RenderPassScope(const RenderPassScope &) = delete;
    RenderPassScope &operator=(const RenderPassScope &) = delete;

private:
    static constexpr size_t kMaxColorAttachments = 8;

    VkCommandBuffer _commandBuffer {VK_NULL_HANDLE};
};

} // namespace graphics

} // namespace reone
