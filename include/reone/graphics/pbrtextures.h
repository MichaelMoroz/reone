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

#include "texture.h"

namespace reone {

namespace graphics {

struct EnvMapDerivedRequest {
    Texture &texture;

    EnvMapDerivedRequest(Texture &texture) :
        texture(texture) {
    }
};

} // namespace graphics

} // namespace reone

template <>
struct std::less<reone::graphics::EnvMapDerivedRequest> {
    bool operator()(const reone::graphics::EnvMapDerivedRequest &lhs,
                    const reone::graphics::EnvMapDerivedRequest &rhs) const {
        return lhs.texture.name() < rhs.texture.name();
    }
};

namespace reone {

namespace graphics {

class IPBRTextures {
public:
    virtual ~IPBRTextures() = default;

    virtual void refresh() = 0;
    virtual void requestEnvMapDerived(EnvMapDerivedRequest request) = 0;
    virtual std::optional<int> findEnvMapDerivedLayer(const std::string &name) = 0;

    virtual Texture &brdf() = 0;
};

} // namespace graphics

} // namespace reone
