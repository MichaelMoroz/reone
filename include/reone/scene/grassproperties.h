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

namespace reone {

namespace graphics {

class Model;
class Texture;

}

namespace scene {

struct GrassProperties {
    float density {0.0f};
    float quadSize {0.0f};
    glm::vec4 probabilities {0.0f};
    std::set<uint32_t> materials;
    graphics::Texture *texture {nullptr};
    /**
     * The area's authored cutout threshold for grass, from the ARE AlphaTest
     * field. Negative where the area authored none, which leaves the blade
     * texture's own header value to decide.
     */
    float alphaTest {-1.0f};
    /**
     * The room's drawn geometry, which is not the geometry grass is scattered
     * over.
     *
     * Placement comes from the model's AABB node - the walkmesh - because that
     * is what carries the surface materials saying where grass may grow. The
     * ground the player sees is a separate and much finer mesh, so a root
     * placed at the walkmesh height lands wherever the two disagree. Measured
     * on Dantooine's estate that is 0.03 world units over open meadow and up
     * to 0.78 over a planter bed, whose soil is drawn as a mound above the
     * flat walkmesh face beneath it.
     *
     * Given the model, the height a face record carries is taken from the
     * drawn surface instead. Null leaves placement on the walkmesh.
     */
    const graphics::Model *groundModel {nullptr};
};

} // namespace scene

} // namespace reone
