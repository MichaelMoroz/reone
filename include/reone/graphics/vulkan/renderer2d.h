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

#include "reone/graphics/commandbuffer.h"
#include "reone/graphics/descriptors.h"
#include "reone/graphics/image.h"
#include "reone/graphics/pipelinecache.h"
#include "../renderer2d.h"

namespace reone {

namespace graphics {

struct LocalUniforms;

class VulkanDevice;
class VulkanResources;
class VulkanUniformRing;

/**
 * Screen-space drawing in Vulkan.
 *
 * The interface is the one the GL backend already implements, unchanged: this
 * is what the seam built in phase 3 was for. Where the GL version changes state
 * and draws, this one selects a pipeline from the cache, writes a slice of the
 * frame's uniform arena and records a draw.
 *
 * Blend mode is a scope in the interface and a pipeline property here, so
 * withBlendMode sets what the next draws select rather than changing anything
 * immediately. Scissor is genuinely dynamic state and is set on the command
 * buffer.
 *
 * A frame must be opened with begin() before any draw and closed with end().
 * The renderer does that; callers see only I2DRenderer.
 */
class Vulkan2DRenderer : public I2DRenderer, boost::noncopyable {
public:
    Vulkan2DRenderer(VulkanDevice &device,
                     IPipelineCache &pipelines,
                     VulkanUniformRing &ring,
                     IDescriptors &descriptors,
                     VulkanResources &resources) :
        _device(device),
        _pipelines(pipelines),
        _ring(ring),
        _descriptors(descriptors),
        _resources(resources) {
    }

    void init() override;
    void deinit() override;

    /**
     * Bind this renderer to the command buffer being recorded. @p extent sets
     * the orthographic projection, in pixels with y down from the top left.
     */
    /** extent is the logical 2D coordinate space (the configured resolution);
        physicalExtent is the actual swapchain size the viewport covers. They
        differ when the OS clamps the window, and conflating them cropped the
        whole frame at 1:1 instead of scaling it. */
    void begin(ICommandBuffer &commandBuffer, glm::ivec2 extent, glm::ivec2 physicalExtent,
               Format colorFormat);
    void end();

    void drawImage(Texture &texture,
                   const glm::vec2 &position,
                   const glm::vec2 &size,
                   const glm::vec4 &color = glm::vec4(1.0f),
                   const glm::mat3x4 &uv = glm::mat3x4(1.0f)) override;

    void drawImage(Texture &texture,
                   const glm::mat4 &transform,
                   const glm::vec4 &color = glm::vec4(1.0f),
                   const glm::mat3x4 &uv = glm::mat3x4(1.0f)) override;

    void drawRect(const glm::vec2 &position,
                  const glm::vec2 &size,
                  const glm::vec4 &color) override;

    void drawFullTargetImage(Texture &texture,
                             const glm::mat3x4 &uv = glm::mat3x4(1.0f)) override;

    void drawText(Font &font,
                  std::string_view text,
                  const glm::vec3 &position,
                  const glm::vec3 &color = glm::vec3(1.0f),
                  TextGravity gravity = TextGravity::CenterCenter) override;

    void withBlendMode(BlendMode mode, const std::function<void()> &block) override;
    void withScissor(const glm::ivec4 &bounds, const std::function<void()> &block) override;

    /** Draws recorded since begin(), for sizing and for tests. */
    int drawCount() const { return _drawCount; }

private:
    VulkanDevice &_device;
    IPipelineCache &_pipelines;
    VulkanUniformRing &_ring;
    IDescriptors &_descriptors;
    VulkanResources &_resources;

    ICommandBuffer *_commandBuffer {nullptr};
    glm::ivec2 _extent {0};
    glm::ivec2 _physicalExtent {0};
    Format _colorFormat {Format::R8G8B8A8Unorm};
    BlendMode _blend {BlendMode::Normal};
    /** Offset of the projection pushed once per begin(), reused by every draw. */
    uint32_t _globalsOffset {0};
    int _drawCount {0};

    /**
     * Bind the pipeline for @p fragmentEntry, write locals, bind both sets and
     * draw @p instances quads. Every draw here is that, with different content.
     */
    void drawQuads(const char *vertexEntry,
                   const char *fragmentEntry,
                   const LocalUniforms &locals,
                   uint32_t textOffset,
                   int instances,
                   const Texture *texture);
};

} // namespace graphics

} // namespace reone
