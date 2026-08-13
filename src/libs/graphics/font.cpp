/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include "reone/graphics/font.h"

#include "reone/graphics/rendering/renderer2d.h"
#include "reone/graphics/texture.h"

namespace reone {

namespace graphics {

void Font::load(std::shared_ptr<Texture> texture) {
    _texture = texture;

    const Texture::Features &features = texture->features();
    _height = features.fontHeight * 100.0f;
    _spacingR = features.spacingR * 100.0f;
    _glyphs.reserve(features.numChars);

    float textureAspect = static_cast<float>(texture->width()) / texture->height();

    for (int i = 0; i < features.numChars; ++i) {
        glm::vec2 ul(features.upperLeftCoords[i]);
        glm::vec2 lr(features.lowerRightCoords[i]);
        float w = lr.x - ul.x;
        float h = ul.y - lr.y;
        float aspect = h != 0.0f ? (w / h) * textureAspect : 0.0f;

        Glyph glyph;
        glyph.ul = std::move(ul);
        glyph.lr = std::move(lr);
        glyph.size = glm::vec2(aspect * _height, _height);

        _glyphs.push_back(std::move(glyph));
    }
}

void Font::render(std::string_view text, const glm::vec3 &position, const glm::vec3 &color, TextGravity gravity, float scale) {
    render(text, position, glm::vec4(color, 1.0f), gravity, scale);
}

void Font::render(std::string_view text, const glm::vec3 &position, const glm::vec4 &color, TextGravity gravity, float scale) {
    _renderer2d.drawText(*this, text, position, color, gravity, scale);
}

glm::vec2 Font::textOffset(std::string_view text, TextGravity gravity, float scale) const {
    float w = measure(text, scale);
    float h = scaledMetric(_height, scale);

    switch (gravity) {
    case TextGravity::LeftCenter:
        return glm::vec2(-w, -0.5f * h);
    case TextGravity::LeftTop:
        return glm::vec2(-w, -h);
    case TextGravity::CenterBottom:
        return glm::vec2(-0.5f * w, 0.0f);
    case TextGravity::CenterTop:
        return glm::vec2(-0.5f * w, -h);
    case TextGravity::RightBottom:
        return glm::vec2(0.0f, 0.0f);
    case TextGravity::RightCenter:
        return glm::vec2(0.0f, -0.5f * h);
    case TextGravity::RightTop:
        return glm::vec2(0.0f, -h);
    case TextGravity::CenterCenter:
    default:
        return glm::vec2(-0.5f * w, -0.5f * h);
    }
}

float Font::measure(std::string_view text, float scale) const {
    float w = 0.0f;
    for (const char &glyph : text) {
        w += glyphAdvance(_glyphs[reinterpret_cast<const unsigned char &>(glyph)], scale);
    }
    return w;
}

} // namespace graphics

} // namespace reone
