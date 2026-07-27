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

#include <variant>

#include "reone/graphics/material.h"

#include "pass.h"

namespace reone {

namespace scene {

struct RegisteredSkin {
    std::vector<glm::mat4> bones;
    std::vector<glm::mat4> prevBones;
};

struct RegisteredDangly {
    std::vector<glm::vec4> positions;
};

struct RegisteredSaber {
    glm::vec4 displacement {0.0f};
};

using RegisteredDeformation =
    std::variant<std::monostate, RegisteredSkin, RegisteredDangly, RegisteredSaber>;

struct RegisteredMesh {
    std::reference_wrapper<graphics::Mesh> mesh;
    graphics::Material material;
    glm::mat4 transform {1.0f};
    glm::mat4 transformInv {1.0f};
    glm::mat4 prevTransform {1.0f};
    RegisteredDeformation deformation;
};

struct RegisteredParticles {
    graphics::Material material;
    glm::ivec2 gridSize {1};
    std::vector<ParticleInstance> instances;
};

struct RegisteredGrass {
    graphics::Material material;
    float radius {0.0f};
    float quadSize {0.0f};
    std::vector<GrassInstance> instances;
};

/**
 * A snapshot of the scene objects submitted to one Vulkan pipeline render.
 *
 * Materials and animated state are owned copies because scene callbacks build
 * them on the stack. Rebuilding the snapshot avoids a retained invalidation
 * contract until one can account for every animated field explicitly.
 */
class RenderRegistry {
public:
    void clear() {
        _meshes.clear();
        _particles.clear();
        _grass.clear();
    }

    void addMesh(graphics::Mesh &mesh,
                 const graphics::Material &material,
                 const glm::mat4 &transform,
                 const glm::mat4 &transformInv,
                 const glm::mat4 &prevTransform,
                 RegisteredDeformation deformation = {}) {
        _meshes.push_back(
            {mesh, material, transform, transformInv, prevTransform, std::move(deformation)});
    }

    void addParticles(const graphics::Material &material,
                      const glm::ivec2 &gridSize,
                      const std::vector<ParticleInstance> &instances) {
        _particles.push_back({material, gridSize, instances});
    }

    void addGrass(const graphics::Material &material,
                  float radius,
                  float quadSize,
                  const std::vector<GrassInstance> &instances) {
        _grass.push_back({material, radius, quadSize, instances});
    }

    const std::vector<RegisteredMesh> &meshes() const { return _meshes; }
    const std::vector<RegisteredParticles> &particles() const { return _particles; }
    const std::vector<RegisteredGrass> &grass() const { return _grass; }

private:
    std::vector<RegisteredMesh> _meshes;
    std::vector<RegisteredParticles> _particles;
    std::vector<RegisteredGrass> _grass;
};

} // namespace scene

} // namespace reone
