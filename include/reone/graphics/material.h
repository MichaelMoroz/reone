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

#include <array>
#include <type_traits>

#include "texture.h"
#include "types.h"
#include "uniforms.h"

namespace reone {

namespace graphics {

class Texture;

enum class MaterialType {
    OpaqueModel,
    TransparentModel,
    Walkmesh,
    Grass,
    Particle
};

enum class MaterialTextureSlot : size_t {
    MainTex,
    Lightmap,
    EnvMap,
    NormalMap,
    BumpMapArray,
    EnvMapCube,
    Count
};

constexpr int materialTextureUnit(MaterialTextureSlot slot) {
    switch (slot) {
    case MaterialTextureSlot::MainTex:
        return TextureUnits::mainTex;
    case MaterialTextureSlot::Lightmap:
        return TextureUnits::lightmap;
    case MaterialTextureSlot::EnvMap:
        return TextureUnits::envMap;
    case MaterialTextureSlot::NormalMap:
        return TextureUnits::normalMap;
    case MaterialTextureSlot::BumpMapArray:
        return TextureUnits::bumpMapArray;
    case MaterialTextureSlot::EnvMapCube:
        return TextureUnits::envMapCube;
    case MaterialTextureSlot::Count:
        break;
    }
    return 0;
}

struct Material {
public:
    static constexpr size_t kNumTextureSlots = static_cast<size_t>(MaterialTextureSlot::Count);

    MaterialType type;
    std::array<Texture *, kNumTextureSlots> textures {};
    glm::mat3x4 uv {1.0f};
    glm::vec4 color {1.0f};
    int bumpMapFrame {0};

    glm::vec3 ambientColor {1.0f};
    glm::vec3 diffuseColor {1.0f};
    glm::vec3 selfIllumColor {0.0f};

    bool staticObject {false};
    /** The authored MDL background-geometry flag: sky domes and backdrops.
        Tracing keys its sky classification on this identity rather than on
        emission luma, which misclassified interior lit panels as sky. */
    bool backgroundGeometry {false};
    /** Index into the registry's curated-material table, resolved by name
        at registration; -1 when the object has no curated record. Carries
        class and material operations both. */
    int curatedIndex {-1};
    bool affectedByShadows {false};
    bool affectedByFog {false};

    std::optional<BlendMode> blending;
    std::optional<FaceCullMode> faceCulling;
    std::optional<PolygonMode> polygonMode;
};

static_assert(std::is_trivially_copyable_v<Material>);

/**
 * Features that describe a material rather than a particular draw path.
 * Keeping this beside Material prevents backend-specific copies from quietly
 * changing which fragment path an otherwise identical material takes.
 */
inline int materialFeatureMask(const Material &material) {
    int mask = 0;
    const auto &textures = material.textures;
    if (const auto *mainTex = textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        switch (mainTex->features().blending) {
        case Texture::Blending::PunchThrough:
            mask |= UniformsFeatureFlags::hashedalphatest;
            break;
        case Texture::Blending::Additive:
            if (!textures[static_cast<size_t>(MaterialTextureSlot::EnvMap)] &&
                !textures[static_cast<size_t>(MaterialTextureSlot::EnvMapCube)]) {
                mask |= UniformsFeatureFlags::premulalpha;
            }
            break;
        default:
            break;
        }
        if (mainTex->features().waterAlpha != -1.0f) {
            mask |= UniformsFeatureFlags::water;
        }
    }
    if (textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)]) {
        mask |= UniformsFeatureFlags::lightmap;
    }
    if (textures[static_cast<size_t>(MaterialTextureSlot::EnvMap)]) {
        mask |= UniformsFeatureFlags::envmap;
    }
    if (textures[static_cast<size_t>(MaterialTextureSlot::EnvMapCube)]) {
        mask |= UniformsFeatureFlags::envmap | UniformsFeatureFlags::envmapcube;
    }
    if (textures[static_cast<size_t>(MaterialTextureSlot::NormalMap)]) {
        mask |= UniformsFeatureFlags::normalmap;
    }
    if (textures[static_cast<size_t>(MaterialTextureSlot::BumpMapArray)]) {
        mask |= UniformsFeatureFlags::bumpmap;
    }
    if (material.staticObject) {
        mask |= UniformsFeatureFlags::staticobj;
    }
    if (material.affectedByShadows) {
        mask |= UniformsFeatureFlags::shadows;
    }
    // A punch-through texture is the format's way of writing "this is a leaf,
    // a frond, a piece of cloth". Those have no thickness, so they are lit from
    // both sides - and they receive shadows whatever the model says, because a
    // cutout that cannot be shadowed reads as a hole cut in the lighting.
    if (mask & UniformsFeatureFlags::hashedalphatest) {
        mask |= UniformsFeatureFlags::thin | UniformsFeatureFlags::shadows;
    }
    if (material.affectedByFog) {
        mask |= UniformsFeatureFlags::fog;
    }
    return mask;
}

} // namespace graphics

} // namespace reone
