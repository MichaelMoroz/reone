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

#include "../types.h"
#include "reone/graphics/rhi/commandbuffer.h"
#include "reone/graphics/rhi/descriptors.h"
#include "reone/graphics/rhi/image.h"
#include "reone/graphics/rhi/pipelinecache.h"
#include "reone/graphics/rhi/resources.h"
#include "reone/graphics/rhi/uniformring.h"

namespace reone {

namespace graphics {

class Font;
class Texture;

/**
 * Everything the game draws in screen space: sprites, filled rectangles, text
 * and full-target images.
 *
 * With IRenderer this is the whole of the backend-neutral drawing API. It is
 * deliberately a sprite and text batcher and nothing more. The scope is set by
 * what the game actually draws in 2D - reticles, minimap, action bar, cursor,
 * GUI borders, text, movie frames - and widening it past that would mean
 * writing a general 2D canvas neither backend needs.
 *
 * Callers previously reached for the OpenGL vocabulary directly: pick a shader
 * program, bind a texture, fill a uniform block, draw a quad mesh. None of
 * those four steps mean anything in the RHI, where the program and the blend
 * state are baked into a pipeline object and the uniforms are a slice of a
 * per-frame buffer.
 *
 * Positions and sizes are in pixels, y down from the top left. They are
 * floating point because several callers compute them continuously: the
 * minimap scrolls under a fixed viewport, and rounding its origin to whole
 * pixels makes it jump a pixel at a time instead of gliding.
 */
class I2DRenderer {
public:
    virtual ~I2DRenderer() = default;

    virtual void init() = 0;
    virtual void deinit() = 0;

    /**
     * A textured quad filling a pixel rect. @p color multiplies the texture,
     * and @p uv transforms the texture coordinates - flipping an arrow, or
     * picking a sub-rect out of an atlas.
     */
    virtual void drawImage(Texture &texture,
                           const glm::vec2 &position,
                           const glm::vec2 &size,
                           const glm::vec4 &color = glm::vec4(1.0f),
                           const glm::mat3x4 &uv = glm::mat3x4(1.0f)) = 0;

    /**
     * A textured quad under an arbitrary transform, for the cases a rect cannot
     * express. Only the minimap's party-leader arrow needs this, because it
     * rotates about its centre.
     */
    virtual void drawImage(Texture &texture,
                           const glm::mat4 &transform,
                           const glm::vec4 &color = glm::vec4(1.0f),
                           const glm::mat3x4 &uv = glm::mat3x4(1.0f)) = 0;

    /** A solid-colour quad filling a pixel rect. */
    virtual void drawRect(const glm::vec2 &position,
                          const glm::vec2 &size,
                          const glm::vec4 &color) = 0;

    /**
     * An image covering the whole render target, bypassing the 2D projection.
     * Movie frames are the only user.
     */
    virtual void drawFullTargetImage(Texture &texture,
                                     const glm::mat3x4 &uv = glm::mat3x4(1.0f)) = 0;

    /**
     * A run of text. @p position is the anchor; @p gravity says which part of
     * the text sits there. The font supplies glyph metrics and the atlas; how
     * the glyphs are submitted is this renderer's business.
     */
    virtual void drawText(Font &font,
                          std::string_view text,
                          const glm::vec3 &position,
                          const glm::vec3 &color = glm::vec3(1.0f),
                          TextGravity gravity = TextGravity::CenterCenter) = 0;

    /**
     * Blending and scissoring apply to the draws inside the block. Scoped
     * rather than per-draw because that is how the call sites are written, and
     * because a backend batching by pipeline wants to know the extent of a run
     * rather than be told again on every quad.
     */
    virtual void withBlendMode(BlendMode mode, const std::function<void()> &block) = 0;
    /**
     * Clip the draws inside the block to @p bounds.
     *
     * XY is the top-left corner, ZW the size, in the same screen coordinates
     * every other call here uses. OpenGL's scissor box is measured from the
     * bottom instead, so that backend flips it; callers should not, and one
     * that did left the minimap clipped entirely off screen.
     */
    virtual void withScissor(const glm::ivec4 &bounds, const std::function<void()> &block) = 0;
};

struct LocalUniforms;

/**
 * Screen-space drawing.
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
class Renderer2D : public I2DRenderer, boost::noncopyable {
public:
    Renderer2D(IPipelineCache &pipelines,
                     IUniformRing &ring,
                     IDescriptors &descriptors,
                     IResources &resources) :
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
    IPipelineCache &_pipelines;
    IUniformRing &_ring;
    IDescriptors &_descriptors;
    IResources &_resources;

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
