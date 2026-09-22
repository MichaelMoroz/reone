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

#include "frustum.h"
#include "types.h"

namespace reone {

namespace graphics {

class Camera {
public:
    Camera(CameraType type) :
        _type(type) {
    }

    virtual ~Camera() = default;

    CameraType type() const { return _type; }
    const glm::mat4 &projection() const { return _projection; }
    const glm::mat4 &projectionInv() const { return _projectionInv; }
    const glm::mat4 &view() const { return _view; }
    const glm::mat4 &viewInv() const { return _viewInv; }
    const glm::vec3 &position() const { return _position; }
    const Frustum &frustum() const { return _frustum; }
    float zNear() const { return _zNear; }
    float zFar() const { return _zFar; }

    glm::vec3 forward() const {
        return -glm::vec3 {_view[0][2],
                           _view[1][2],
                           _view[2][2]};
    }

    void setView(glm::mat4 view) {
        _view = std::move(view);
        _viewInv = glm::inverse(_view);
        _position = _viewInv[3];
        updateFrustum();
    }

protected:
    glm::mat4 _projection {1.0f};
    glm::mat4 _projectionInv {1.0f};
    glm::mat4 _view {1.0f};
    glm::mat4 _viewInv {1.0f};
    glm::vec3 _position {0.0f};
    float _zNear {0.0f};
    float _zFar {0.0f};

    void setProjection(glm::mat4 projection) {
        _projection = std::move(projection);
        _projectionInv = glm::inverse(_projection);
        updateFrustum();
    }

private:
    Frustum _frustum;
    CameraType _type;

    void updateFrustum();
};

} // namespace graphics

} // namespace reone
