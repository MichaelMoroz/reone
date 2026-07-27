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
#include "types.h"
#include "uniforms.h"

namespace reone {

namespace graphics {

class Texture;

enum class MaterialType {
    OpaqueModel,
    TransparentModel,
    DirLightShadow,
    PointLightShadow,
    Walkmesh,
    Grass,
    Particle
};

struct Material {
public:
    using TextureUnit = int;
    using TextureUnitToTexture = std::unordered_map<TextureUnit, std::reference_wrapper<Texture>>;

    MaterialType type;
    TextureUnitToTexture textures;
    glm::mat3x4 uv {1.0f};
    glm::vec4 color {1.0f};
    int bumpMapFrame {0};

    glm::vec3 ambientColor {1.0f};
    glm::vec3 diffuseColor {1.0f};
    glm::vec3 selfIllumColor {0.0f};

    bool staticObject {false};
    bool affectedByShadows {false};
    bool affectedByFog {false};

    std::optional<BlendMode> blending;
    std::optional<FaceCullMode> faceCulling;
    std::optional<PolygonMode> polygonMode;
};

/**
 * Features that describe a material rather than a particular draw path.
 * Keeping this beside Material prevents backend-specific copies from quietly
 * changing which fragment path an otherwise identical material takes.
 */
inline int materialFeatureMask(const Material &material) {
    int mask = 0;
    const auto &textures = material.textures;
    if (auto it = textures.find(TextureUnits::mainTex); it != textures.end()) {
        const auto &mainTex = it->second.get();
        switch (mainTex.features().blending) {
        case Texture::Blending::PunchThrough:
            mask |= UniformsFeatureFlags::hashedalphatest;
            break;
        case Texture::Blending::Additive:
            if (textures.count(TextureUnits::envMap) == 0 &&
                textures.count(TextureUnits::envMapCube) == 0) {
                mask |= UniformsFeatureFlags::premulalpha;
            }
            break;
        default:
            break;
        }
        if (mainTex.features().waterAlpha != -1.0f) {
            mask |= UniformsFeatureFlags::water;
        }
    }
    if (textures.count(TextureUnits::lightmap) > 0) {
        mask |= UniformsFeatureFlags::lightmap;
    }
    if (textures.count(TextureUnits::envMap) > 0) {
        mask |= UniformsFeatureFlags::envmap;
    }
    if (textures.count(TextureUnits::envMapCube) > 0) {
        mask |= UniformsFeatureFlags::envmap | UniformsFeatureFlags::envmapcube;
    }
    if (textures.count(TextureUnits::normalMap) > 0) {
        mask |= UniformsFeatureFlags::normalmap;
    }
    if (textures.count(TextureUnits::bumpMapArray) > 0) {
        mask |= UniformsFeatureFlags::bumpmap;
    }
    if (material.staticObject) {
        mask |= UniformsFeatureFlags::staticobj;
    }
    if (material.affectedByShadows) {
        mask |= UniformsFeatureFlags::shadows;
    }
    if (material.affectedByFog) {
        mask |= UniformsFeatureFlags::fog;
    }
    return mask;
}

} // namespace graphics

} // namespace reone
