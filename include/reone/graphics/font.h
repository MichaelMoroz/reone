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

#pragma once

#include "types.h"

namespace reone {

namespace graphics {

class I2DRenderer;
class Texture;

/**
 * A bitmap font: a glyph atlas and the metrics to lay text out in it.
 *
 * Font describes text; it does not draw it. Submitting the glyphs belongs to
 * I2DRenderer, which knows what a draw is on the current backend.
 */
class Font {
public:
    struct Glyph {
        glm::vec2 ul {0.0f};
        glm::vec2 lr {0.0f};
        glm::vec2 size {0.0f};
    };

    Font(I2DRenderer &renderer2d) :
        _renderer2d(renderer2d) {
    }

    void load(std::shared_ptr<Texture> texture);

    /** Convenience for the many call sites that hold a font and want it drawn. */
    void render(
        std::string_view text,
        const glm::vec3 &position,
        const glm::vec3 &color = glm::vec3(1.0f, 1.0f, 1.0f),
        TextGravity align = TextGravity::CenterCenter);

    float measure(std::string_view text) const;

    /**
     * Where to start drawing so that @p text sits at the anchor the way
     * @p gravity asks for.
     */
    glm::vec2 textOffset(std::string_view text, TextGravity gravity) const;

    float height() const { return _height; }

    const std::vector<Glyph> &glyphs() const { return _glyphs; }

    Texture &texture() { return *_texture; }

private:
    std::shared_ptr<Texture> _texture;
    float _height {0.0f};
    std::vector<Glyph> _glyphs;

    I2DRenderer &_renderer2d;
};

} // namespace graphics

} // namespace reone
