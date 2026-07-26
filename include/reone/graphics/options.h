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

struct GraphicsOptions {
    int width {1024};
    int height {768};
    int winScale {100};
    bool fullscreen {false};
    bool vsync {true};
    bool grass {true};
    bool pbr {true};
    bool ssao {true};
    bool ssr {true};
    bool fxaa {true};
    bool sharpen {true};
    /**
     * Offset the projection by a sub-pixel jitter each frame. Motion vectors are
     * produced regardless; this only controls the jitter itself, and is off by
     * default because nothing resolves it yet.
     */
    bool taaJitter {false};
    /**
     * Build the opaque model program from slang/pbr_opaque_model.slang instead of
     * the hand-written GLSL, so the transpiler's output can be compared against
     * it in one binary.
     */
    bool slangShaders {false};
    TextureQuality textureQuality {TextureQuality::High};
    int shadowResolution {2048};
    int anisotropicFiltering {2};
    float drawDistance {kDefaultObjectDrawDistance};
};

} // namespace graphics

} // namespace reone
