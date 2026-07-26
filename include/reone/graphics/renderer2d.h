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

#include "types.h"

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
 * those four steps mean anything in Vulkan, where the program and the blend
 * state are baked into a pipeline object and the uniforms are a slice of a
 * per-frame buffer.
 *
 * Positions and sizes are in pixels, y down from the top left.
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
                           const glm::ivec2 &position,
                           const glm::ivec2 &size,
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
    virtual void drawRect(const glm::ivec2 &position,
                          const glm::ivec2 &size,
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
    virtual void withScissor(const glm::ivec4 &bounds, const std::function<void()> &block) = 0;
};

} // namespace graphics

} // namespace reone
