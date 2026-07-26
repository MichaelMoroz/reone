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

#include "reone/graphics/renderer/gl2d.h"

#include "reone/graphics/context.h"
#include "reone/graphics/font.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/shaderregistry.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"

namespace reone {

namespace graphics {

static glm::mat4 rectTransform(const glm::vec2 &position, const glm::vec2 &size) {
    auto transform = glm::translate(glm::vec3(position, 0.0f));
    transform *= glm::scale(glm::vec3(size, 1.0f));
    return transform;
}

void GL2DRenderer::init() {
}

void GL2DRenderer::deinit() {
}

void GL2DRenderer::drawImage(Texture &texture,
                             const glm::vec2 &position,
                             const glm::vec2 &size,
                             const glm::vec4 &color,
                             const glm::mat3x4 &uv) {
    drawImage(texture, rectTransform(position, size), color, uv);
}

void GL2DRenderer::drawImage(Texture &texture,
                             const glm::mat4 &transform,
                             const glm::vec4 &color,
                             const glm::mat3x4 &uv) {
    _uniforms.setLocals([&transform, &color, &uv](auto &locals) {
        locals.reset();
        locals.model = transform;
        locals.color = color;
        locals.uv = uv;
    });
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::mvpTexture));
    _context.bindTexture(texture, TextureUnits::mainTex);
    _meshRegistry.get(MeshName::quad).draw(_statistic);
}

void GL2DRenderer::drawRect(const glm::vec2 &position,
                            const glm::vec2 &size,
                            const glm::vec4 &color) {
    auto transform = rectTransform(position, size);
    _uniforms.setLocals([&transform, &color](auto &locals) {
        locals.reset();
        locals.model = transform;
        locals.color = color;
    });
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::mvpColor));
    _meshRegistry.get(MeshName::quad).draw(_statistic);
}

void GL2DRenderer::drawFullTargetImage(Texture &texture, const glm::mat3x4 &uv) {
    _uniforms.setLocals([&uv](auto &locals) {
        locals.reset();
        locals.uv = uv;
    });
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::ndcTexture));
    _context.bindTexture(texture, TextureUnits::mainTex);
    _meshRegistry.get(MeshName::quadNDC).draw(_statistic);
}

void GL2DRenderer::drawText(Font &font,
                            std::string_view text,
                            const glm::vec3 &position,
                            const glm::vec3 &color,
                            TextGravity gravity) {
    if (text.empty()) {
        return;
    }
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::text));
    _context.bindTexture(font.texture(), TextureUnits::mainTex);

    _uniforms.setLocals([&color](auto &locals) {
        locals.reset();
        locals.color = glm::vec4(color, 1.0f);
    });

    // The text uniform block holds a fixed number of glyphs, so a long run is
    // drawn as several instanced draws rather than one.
    int numBlocks = static_cast<int>(text.size()) / kMaxTextChars;
    if (text.size() % kMaxTextChars > 0) {
        ++numBlocks;
    }
    glm::vec3 offset(font.textOffset(text, gravity), 0.0f);
    for (int i = 0; i < numBlocks; ++i) {
        int numChars = glm::min(kMaxTextChars, static_cast<int>(text.size()) - i * kMaxTextChars);
        drawTextLine(font, text.substr(i * kMaxTextChars, numChars), position, offset);
    }
}

void GL2DRenderer::drawTextLine(Font &font,
                                std::string_view line,
                                const glm::vec3 &position,
                                glm::vec3 &offset) {
    if (line.empty()) {
        return;
    }
    const auto &glyphs = font.glyphs();
    _uniforms.setText([&glyphs, &line, &position, &offset](auto &uniforms) {
        for (int j = 0; j < line.size(); ++j) {
            const auto &glyph = glyphs[static_cast<unsigned char>(line[j])];

            uniforms.chars[j].posScale = glm::vec4(
                position.x + offset.x,
                position.y + offset.y,
                glyph.size.x,
                glyph.size.y);
            uniforms.chars[j].uv = glm::vec4(
                glyph.ul.x,
                glyph.lr.y,
                glyph.lr.x - glyph.ul.x,
                glyph.ul.y - glyph.lr.y);

            offset.x += glyph.size.x;
        }
    });
    _meshRegistry.get(MeshName::quad).drawInstanced(line.size(), _statistic);
}

void GL2DRenderer::withBlendMode(BlendMode mode, const std::function<void()> &block) {
    _context.withBlendMode(mode, block);
}

void GL2DRenderer::withScissor(const glm::ivec4 &bounds, const std::function<void()> &block) {
    _context.withScissorTest(bounds, block);
}

} // namespace graphics

} // namespace reone
