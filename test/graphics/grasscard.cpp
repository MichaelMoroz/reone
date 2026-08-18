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

#include <gtest/gtest.h>

#include "reone/graphics/grasscard.h"
#include "reone/graphics/texture.h"

#include <array>
#include <cmath>

using namespace reone;
using namespace reone::graphics;

namespace {

std::shared_ptr<Texture> makeTexture(int width, int height, PixelFormat format, const std::array<std::vector<bool>, 4> &masks) {
    auto pixels = std::make_shared<ByteBuffer>(static_cast<size_t>(width) * height * 4, '\0');
    const int cellWidth = width / 2;
    const int cellHeight = height / 2;
    for (int variant = 0; variant < 4; ++variant) {
        for (int y = 0; y < cellHeight; ++y) {
            for (int x = 0; x < cellWidth; ++x) {
                const int textureX = (variant % 2) * cellWidth + x;
                const int textureY = (variant / 2) * cellHeight + y;
                (*pixels)[4 * (textureY * width + textureX) + 3] =
                    masks[variant][y * cellWidth + x] ? static_cast<char>(255) : 0;
            }
        }
    }
    auto texture = std::make_shared<Texture>("grass-test", TextureType::TwoDim, Texture::Properties {});
    texture->setPixels(width, height, format, Texture::Layer {std::move(pixels)});
    return texture;
}

std::array<std::vector<bool>, 4> masks(int size) {
    std::array<std::vector<bool>, 4> result;
    for (auto &mask : result)
        mask.resize(static_cast<size_t>(size) * size, false);
    return result;
}

std::array<std::vector<bool>, 4> repeatMask(const std::vector<bool> &mask) {
    return {mask, mask, mask, mask};
}

float cross(const glm::vec2 &a, const glm::vec2 &b) {
    return a.x * b.y - a.y * b.x;
}

bool contains(const GrassCardVariant &variant, glm::vec2 point) {
    for (size_t i = 0; i < variant.indices.size(); i += 3) {
        const auto &a = variant.vertices[variant.indices[i]];
        const auto &b = variant.vertices[variant.indices[i + 1]];
        const auto &c = variant.vertices[variant.indices[i + 2]];
        const glm::vec2 pa {a.z, a.w};
        const glm::vec2 pb {b.z, b.w};
        const glm::vec2 pc {c.z, c.w};
        const float first = cross(pb - pa, point - pa);
        const float second = cross(pc - pb, point - pb);
        const float third = cross(pa - pc, point - pc);
        if ((first >= -1e-5f && second >= -1e-5f && third >= -1e-5f) ||
            (first <= 1e-5f && second <= 1e-5f && third <= 1e-5f))
            return true;
    }
    return false;
}

void expectOpaqueCentresCovered(const Texture &texture, const GrassCardAtlas &atlas,
                                const std::array<std::vector<bool>, 4> &mask) {
    const int cellWidth = texture.width() / 2;
    const int cellHeight = texture.height() / 2;
    for (int variant = 0; variant < 4; ++variant) {
        for (int y = 0; y < cellHeight; ++y) {
            for (int x = 0; x < cellWidth; ++x) {
                if (!mask[variant][y * cellWidth + x])
                    continue;
                EXPECT_TRUE(contains(atlas.variants[variant], {
                    (x + 0.5f) / cellWidth, (y + 0.5f) / cellHeight}));
            }
        }
    }
}

} // namespace

TEST(GrassCard, fits_every_shape_conservatively) {
    constexpr int kSize = 16;
    auto mask = masks(kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const int dx = x - 8;
            const int dy = y - 8;
            mask[0][y * kSize + x] = dx * dx + dy * dy <= 25;
            mask[1][y * kSize + x] = std::abs(x - y) <= 1 && x >= 3 && x <= 12;
            mask[2][y * kSize + x] = x >= 7 && x <= 8 && y >= 2 && y <= 14;
            mask[3][y * kSize + x] = (x <= 4 && y <= 4) || (x >= 11 && y >= 10);
        }
    }
    const auto texture = makeTexture(2 * kSize, 2 * kSize, PixelFormat::RGBA8, mask);
    for (const auto shape : {GrassCardShape::Aabb, GrassCardShape::Obb, GrassCardShape::Kgon, GrassCardShape::Bgrid}) {
        for (const int value : {3, 5, 8, 16}) {
            GrassCardParams params;
            params.shape = shape;
            params.sides = value;
            params.grid = value;
            const auto atlas = fitGrassCards(*texture, params);
            EXPECT_FLOAT_EQ(1.0f, atlas.coverage);
            expectOpaqueCentresCovered(*texture, atlas, mask);
        }
    }
}

TEST(GrassCard, keeps_minimum_area_box_no_larger_than_aabb) {
    constexpr int kSize = 16;
    auto mask = masks(kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const int dx = x - 8;
            const int dy = y - 8;
            mask[0][y * kSize + x] = dx * dx + dy * dy <= 25;
            mask[1][y * kSize + x] = std::abs(x - y) <= 1 && x >= 3 && x <= 12;
            mask[2][y * kSize + x] = x >= 7 && x <= 8 && y >= 2 && y <= 14;
            mask[3][y * kSize + x] = (x <= 4 && y <= 4) || (x >= 11 && y >= 10);
        }
    }
    for (const auto &oneMask : mask) {
        const auto texture = makeTexture(2 * kSize, 2 * kSize, PixelFormat::RGBA8, repeatMask(oneMask));
        GrassCardParams params;
        params.shape = GrassCardShape::Aabb;
        const auto aabb = fitGrassCards(*texture, params);
        params.shape = GrassCardShape::Obb;
        const auto obb = fitGrassCards(*texture, params);
        EXPECT_LE(obb.areaFraction, aabb.areaFraction + 1e-5f);
    }
}

TEST(GrassCard, removes_empty_area) {
    constexpr int kSize = 16;
    auto mask = masks(kSize);
    for (int y = 2; y < 14; ++y) {
        for (int x = 2; x < 14; ++x)
            mask[0][y * kSize + x] = std::abs(x - y) <= 1;
    }
    const auto texture = makeTexture(2 * kSize, 2 * kSize, PixelFormat::RGBA8, mask);
    GrassCardParams params;
    params.shape = GrassCardShape::Quad;
    const auto quad = fitGrassCards(*texture, params);
    params.shape = GrassCardShape::Aabb;
    const auto aabb = fitGrassCards(*texture, params);
    params.shape = GrassCardShape::Obb;
    const auto obb = fitGrassCards(*texture, params);
    params.shape = GrassCardShape::Kgon;
    params.sides = 5;
    const auto kgon = fitGrassCards(*texture, params);
    params.shape = GrassCardShape::Bgrid;
    params.grid = 16;
    const auto bgrid = fitGrassCards(*texture, params);

    EXPECT_LT(aabb.areaFraction, quad.areaFraction);
    EXPECT_LE(obb.areaFraction, aabb.areaFraction + 1e-5f);
    EXPECT_LT(kgon.areaFraction, aabb.areaFraction);
    EXPECT_LT(bgrid.areaFraction, obb.areaFraction);
    EXPECT_LT(bgrid.areaFraction, kgon.areaFraction);
}

// Card y runs the same way as texture v, because the grass image is stored
// bottom-up and the cardboard quad this replaces mapped uv.v straight from its
// height. Asserting the opposite is what put the field upside down, so this
// pins the direction against the row the mask actually occupies.
TEST(GrassCard, maps_texture_v_directly_to_card_y) {
    constexpr int kSize = 8;
    auto mask = masks(kSize);
    for (int y = 4; y < kSize; ++y) {
        for (int x = 2; x < 6; ++x)
            mask[0][y * kSize + x] = true;
    }
    const auto texture = makeTexture(2 * kSize, 2 * kSize, PixelFormat::RGBA8, mask);
    GrassCardParams params;
    params.shape = GrassCardShape::Aabb;
    const auto atlas = fitGrassCards(*texture, params);
    for (const auto &vertex : atlas.variants[0].vertices) {
        EXPECT_GE(vertex.y, 0.5f);
        // Position and UV must agree, or the outline is fitted to one half of
        // the quadrant and textured from the other.
        EXPECT_FLOAT_EQ(vertex.y, vertex.w);
    }
}

TEST(GrassCard, pads_grid_variants_with_real_triangles) {
    constexpr int kSize = 8;
    auto mask = masks(kSize);
    for (int y = 1; y < 7; ++y)
        mask[0][y * kSize + 1] = true;
    for (int x = 1; x < 7; ++x)
        mask[1][2 * kSize + x] = true;
    mask[2][1 * kSize + 1] = true;
    mask[2][6 * kSize + 6] = true;
    for (int y = 1; y < 7; ++y) {
        for (int x = 1; x < 7; ++x)
            mask[3][y * kSize + x] = (x + y) % 2 == 0;
    }
    const auto texture = makeTexture(2 * kSize, 2 * kSize, PixelFormat::RGBA8, mask);
    GrassCardParams params;
    params.shape = GrassCardShape::Bgrid;
    params.grid = 8;
    const auto atlas = fitGrassCards(*texture, params);
    for (const auto &variant : atlas.variants) {
        EXPECT_EQ(atlas.vertsPerCard, variant.vertices.size());
        EXPECT_EQ(atlas.trisPerCard * 3, variant.indices.size());
        for (size_t i = 0; i < variant.indices.size(); i += 3) {
            EXPECT_NE(variant.indices[i], variant.indices[i + 1]);
            EXPECT_NE(variant.indices[i], variant.indices[i + 2]);
            EXPECT_NE(variant.indices[i + 1], variant.indices[i + 2]);
        }
    }
}

TEST(GrassCard, keeps_quad_for_textures_without_alpha) {
    constexpr int kSize = 8;
    auto mask = masks(kSize);
    const auto texture = makeTexture(2 * kSize, 2 * kSize, PixelFormat::RGB8, mask);
    GrassCardParams params;
    params.shape = GrassCardShape::Bgrid;
    params.grid = 16;
    const auto atlas = fitGrassCards(*texture, params);
    EXPECT_EQ(4u, atlas.vertsPerCard);
    EXPECT_EQ(2u, atlas.trisPerCard);
    EXPECT_FLOAT_EQ(1.0f, atlas.areaFraction);
    for (const auto &variant : atlas.variants) {
        EXPECT_EQ(4u, variant.vertices.size());
        EXPECT_EQ(6u, variant.indices.size());
    }
}

TEST(GrassCard, keeps_quad_for_unsupported_alpha_format) {
    constexpr int kSize = 8;
    auto mask = masks(kSize);
    const auto texture = makeTexture(2 * kSize, 2 * kSize, PixelFormat::RGBA16F, mask);
    GrassCardParams params;
    params.shape = GrassCardShape::Bgrid;
    const auto atlas = fitGrassCards(*texture, params);
    EXPECT_EQ(4u, atlas.vertsPerCard);
    EXPECT_EQ(2u, atlas.trisPerCard);
    EXPECT_FLOAT_EQ(1.0f, atlas.areaFraction);
}

TEST(GrassCard, shape_names_round_trip) {
    for (const auto shape : {GrassCardShape::Quad, GrassCardShape::Aabb, GrassCardShape::Obb,
                             GrassCardShape::Kgon, GrassCardShape::Bgrid})
        EXPECT_EQ(shape, parseGrassCardShape(grassCardShapeName(shape)));
}
