/*
 * Copyright (c) 2026 The reone project contributors
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
#include <cstdint>
#include <string>
#include <vector>

namespace reone {

namespace graphics {

class Texture;

/** How a grass card's outline is fitted to the opaque part of its variant. */
enum class GrassCardShape {
    Quad,
    Aabb,
    Obb,
    Kgon,
    Bgrid
};

/**
 * One variant's outline, in the unit card frame.
 *
 * Positions are bottom-anchored to match the cardboard quad the merge kernel
 * already builds: x in [-0.5, 0.5] across the card and y in [0, 1] up it.
 * UVs are in [0, 1] within the variant's own quadrant; the merge kernel
 * applies the atlas offset.
 *
 * Each vertex is (x, y, u, v). Indices are local to this variant and index
 * `vertices`, three per triangle.
 */
struct GrassCardVariant {
    std::vector<glm::vec4> vertices;
    std::vector<uint32_t> indices;
};

/** The four variants, padded to a common size. */
struct GrassCardAtlas {
    std::array<GrassCardVariant, 4> variants;
    uint32_t vertsPerCard {0};
    uint32_t trisPerCard {0};
    /** Opaque texels covered by the fitted outlines, over those in the atlas. */
    float coverage {1.0f};
    /** Fitted area over the four full quadrants. */
    float areaFraction {1.0f};
};

struct GrassCardParams {
    GrassCardShape shape {GrassCardShape::Quad};
    /** Sides of the k-gon, 3..16. Ignored by other shapes. */
    int sides {5};
    /** Cells across the grid, 2..32. Ignored by other shapes. */
    int grid {8};
    /** Alpha at or above which a texel is opaque, in [0,1]. */
    float alphaTest {0.5f};
};

/**
 * Fit card outlines to a grass texture's alpha.
 *
 * Every fit is conservative: it contains every opaque texel of its variant.
 */
GrassCardAtlas fitGrassCards(const Texture &texture, const GrassCardParams &params);

const char *grassCardShapeName(GrassCardShape shape);
GrassCardShape parseGrassCardShape(const std::string &value);

} // namespace graphics

} // namespace reone
