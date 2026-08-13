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

#include "reone/gui/control/progressbar.h"

#include "reone/graphics/rendering/renderer2d.h"
#include "reone/graphics/texture.h"
#include "reone/gui/gui.h"
#include "reone/resource/gff.h"
#include "reone/resource/provider/textures.h"

using namespace reone::graphics;
using namespace reone::resource;

namespace reone {

namespace gui {

void ProgressBar::load(const resource::generated::GUI_BASECONTROL &gui, bool protoItem) {
    Control::load(gui, protoItem);

    auto &controlStruct = *static_cast<const resource::generated::GUI_CONTROLS *>(&gui);
    _startFromLeft = controlStruct.STARTFROMLEFT != 0;
    if (controlStruct.PROGRESS) {
        _progress.fill = _resourceSvc.textures.get(controlStruct.PROGRESS->FILL, TextureUsage::GUI);
        _progress.color = controlStruct.PROGRESS->COLOR;
    }
}

void ProgressBar::render(const glm::ivec2 &screenSize,
                         const glm::ivec2 &offset,
                         I2DRenderer &renderer2d) {
    if (!_visible) {
        return;
    }
    // The authored backing - the same capsule art tinted with the border
    // colour - draws at full size behind the cropped fill, so a partially
    // filled bar keeps its full silhouette.
    Control::render(screenSize, offset, renderer2d);
    if (_value == 0 || !_progress.fill) {
        return;
    }
    // The fill fraction follows the bar's long axis, keeping the full
    // authored size on the cross axis, and crops the art to the visible
    // fraction instead of squashing it. Tall bars - the party vitality and
    // Force columns - grow from the bottom; wide bars anchor to their
    // authored start edge.
    float fraction = _value / 100.0f;
    glm::mat3x4 uv(1.0f);
    // The reference image API takes ivec2. Convert each completed position and
    // size expression independently to preserve its truncation toward zero.
    if (_extent.height > _extent.width) {
        float h = _extent.height * fraction;
        uv[1][1] = fraction;
        uv[2][1] = 0.0f;
        glm::ivec2 fillPosition {
            _extent.left + offset.x,
            static_cast<int>(_extent.top + _extent.height - h + offset.y)};
        glm::ivec2 fillSize {_extent.width, static_cast<int>(h)};
        renderer2d.drawImage(
            *_progress.fill,
            glm::vec2(fillPosition),
            glm::vec2(fillSize),
            glm::vec4(_progress.color, 1.0f),
            uv);
    } else {
        float w = _extent.width * fraction;
        float left = _startFromLeft ? _extent.left : _extent.left + _extent.width - w;
        uv[0][0] = fraction;
        uv[2][0] = _startFromLeft ? 0.0f : 1.0f - fraction;
        glm::ivec2 fillPosition {
            static_cast<int>(left + offset.x),
            _extent.top + offset.y};
        glm::ivec2 fillSize {static_cast<int>(w), _extent.height};
        renderer2d.drawImage(
            *_progress.fill,
            glm::vec2(fillPosition),
            glm::vec2(fillSize),
            glm::vec4(_progress.color, 1.0f),
            uv);
    }
}

void ProgressBar::setValue(int value) {
    if (value < 0 || value > 100) {
        throw std::out_of_range("value out of range: " + std::to_string(value));
    }
    _value = value;
}

} // namespace gui

} // namespace reone
