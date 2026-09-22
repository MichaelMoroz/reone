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

#include "reone/graphics/vulkan/renderpass.h"

#include <stdexcept>

namespace reone {

namespace graphics {

RenderPassScope::RenderPassScope(
    VkCommandBuffer commandBuffer,
    glm::ivec2 extent,
    std::initializer_list<RenderPassAttachment> colorAttachments,
    std::optional<RenderPassAttachment> depthAttachment,
    uint32_t viewMask,
    bool invertedViewport) :
    _commandBuffer(commandBuffer) {
    if (colorAttachments.size() > kMaxColorAttachments) {
        throw std::invalid_argument("Render pass has too many color attachments");
    }

    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> colors {};
    size_t colorCount = 0;
    for (const auto &attachment : colorAttachments) {
        auto &info = colors[colorCount++];
        info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        info.imageView = attachment.imageView;
        info.imageLayout = attachment.imageLayout;
        info.loadOp = attachment.loadOp;
        info.storeOp = attachment.storeOp;
        info.clearValue = attachment.clearValue;
    }

    VkRenderingAttachmentInfo depth {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depthAttachment) {
        depth.imageView = depthAttachment->imageView;
        depth.imageLayout = depthAttachment->imageLayout;
        depth.loadOp = depthAttachment->loadOp;
        depth.storeOp = depthAttachment->storeOp;
        depth.clearValue = depthAttachment->clearValue;
    }

    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(extent.x),
                                   static_cast<uint32_t>(extent.y)};
    rendering.layerCount = 1;
    rendering.viewMask = viewMask;
    rendering.colorAttachmentCount = static_cast<uint32_t>(colorCount);
    rendering.pColorAttachments = colors.data();
    rendering.pDepthAttachment = depthAttachment ? &depth : nullptr;

    VkViewport viewport {0.0f,
                         invertedViewport ? static_cast<float>(extent.y) : 0.0f,
                         static_cast<float>(extent.x),
                         invertedViewport ? -static_cast<float>(extent.y)
                                          : static_cast<float>(extent.y),
                         0.0f,
                         1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(extent.x),
                               static_cast<uint32_t>(extent.y)}};
    vkCmdBeginRendering(_commandBuffer, &rendering);
    vkCmdSetViewport(_commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(_commandBuffer, 0, 1, &scissor);
}

RenderPassScope::~RenderPassScope() {
    vkCmdEndRendering(_commandBuffer);
}

} // namespace graphics

} // namespace reone
