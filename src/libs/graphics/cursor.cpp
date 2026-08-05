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

#include "reone/graphics/cursor.h"

#include "reone/graphics/texture.h"
#include "reone/graphics/rendering/renderer2d.h"

namespace reone {

namespace graphics {

void Cursor::render() {
    std::shared_ptr<Texture> texture(_pressed ? _down : _up);
    _renderer2d.withBlendMode(BlendMode::Normal, [this, &texture]() {
        _renderer2d.drawImage(*texture, _position, {texture->width(), texture->height()});
    });
}

} // namespace graphics

} // namespace reone
