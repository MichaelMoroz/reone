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

#include "../renderer2d.h"

namespace reone {

namespace graphics {

class IContext;
class IMeshRegistry;
class IShaderRegistry;
class IStatistic;
class IUniforms;

/**
 * Screen-space drawing in OpenGL: a shader program per primitive kind, a unit
 * quad mesh, and immediate state changes.
 *
 * Not a batcher despite the interface allowing one - each call is its own draw,
 * exactly as the open-coded call sites were. Batching here would buy nothing:
 * the GL backend is frozen, and the frame is a few dozen quads.
 */
class GL2DRenderer : public I2DRenderer, boost::noncopyable {
public:
    GL2DRenderer(
        IContext &context,
        IMeshRegistry &meshRegistry,
        IShaderRegistry &shaderRegistry,
        IStatistic &statistic,
        IUniforms &uniforms) :
        _context(context),
        _meshRegistry(meshRegistry),
        _shaderRegistry(shaderRegistry),
        _statistic(statistic),
        _uniforms(uniforms) {
    }

    void init() override;
    void deinit() override;

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

private:
    IContext &_context;
    IMeshRegistry &_meshRegistry;
    IShaderRegistry &_shaderRegistry;
    IStatistic &_statistic;
    IUniforms &_uniforms;

    /** One block of at most kMaxTextChars glyphs, drawn instanced. */
    void drawTextLine(Font &font,
                      std::string_view line,
                      const glm::vec3 &position,
                      glm::vec3 &offset);
};

} // namespace graphics

} // namespace reone
