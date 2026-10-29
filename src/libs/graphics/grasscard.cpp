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

#include "reone/graphics/grasscard.h"

#include "reone/graphics/dxtutil.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/textureutil.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace reone {

namespace graphics {

namespace {

constexpr float kEpsilon = 1e-5f;

using Point = glm::vec2;

struct VariantMask {
    int width;
    int height;
    std::vector<Point> centres;
    Point min {1.0f};
    Point max {0.0f};
};

void appendPolygon(GrassCardVariant &variant, const std::vector<Point> &points) {
    const auto firstVertex = static_cast<uint32_t>(variant.vertices.size());
    for (const auto &point : points)
        // Card y and texture v run the same way, not opposite ways. The
        // cardboard quad this replaces mapped uv.v straight from its height
        // (`uv = float2(x + 0.5, y)`, y running 0 at the base to 1 at the top),
        // because the grass image is stored bottom-up. Flipping here on the
        // usual assumption that v = 0 is the top row mirrors every fitted
        // outline against the blades it was fitted to, and the field comes out
        // upside down.
        variant.vertices.emplace_back(point.x - 0.5f, point.y, point.x, point.y);
    for (uint32_t i = 1; i + 1 < points.size(); ++i) {
        variant.indices.push_back(firstVertex);
        variant.indices.push_back(firstVertex + i);
        variant.indices.push_back(firstVertex + i + 1);
    }
}

GrassCardVariant makeQuad(Point min = {0.0f, 0.0f}, Point max = {1.0f, 1.0f}) {
    GrassCardVariant result;
    appendPolygon(result, {{min.x, min.y}, {max.x, min.y}, {max.x, max.y}, {min.x, max.y}});
    return result;
}

float cross(const Point &a, const Point &b) {
    return a.x * b.y - a.y * b.x;
}

std::vector<Point> convexHull(std::vector<Point> points) {
    std::sort(points.begin(), points.end(), [](const Point &a, const Point &b) {
        return a.x == b.x ? a.y < b.y : a.x < b.x;
    });
    points.erase(std::unique(points.begin(), points.end(), [](const Point &a, const Point &b) {
        return a.x == b.x && a.y == b.y;
    }), points.end());
    if (points.size() < 3)
        return points;

    std::vector<Point> hull;
    hull.reserve(points.size() * 2);
    for (const auto &point : points) {
        while (hull.size() >= 2 && cross(hull.back() - hull[hull.size() - 2], point - hull.back()) <= 0.0f)
            hull.pop_back();
        hull.push_back(point);
    }
    const size_t lowerSize = hull.size();
    for (size_t i = points.size() - 1; i-- > 0;) {
        const auto &point = points[i];
        while (hull.size() > lowerSize && cross(hull.back() - hull[hull.size() - 2], point - hull.back()) <= 0.0f)
            hull.pop_back();
        hull.push_back(point);
    }
    hull.pop_back();
    return hull;
}

std::vector<Point> texelHull(const VariantMask &mask) {
    std::vector<Point> corners;
    corners.reserve(mask.centres.size() * 4);
    const Point halfTexel {0.5f / mask.width, 0.5f / mask.height};
    for (const auto &centre : mask.centres) {
        corners.push_back(centre + Point {-halfTexel.x, -halfTexel.y});
        corners.push_back(centre + Point {halfTexel.x, -halfTexel.y});
        corners.push_back(centre + Point {halfTexel.x, halfTexel.y});
        corners.push_back(centre + Point {-halfTexel.x, halfTexel.y});
    }
    return convexHull(std::move(corners));
}

bool pointInTriangle(const Point &point, const glm::vec4 &a, const glm::vec4 &b, const glm::vec4 &c) {
    const Point pa {a.z, a.w};
    const Point pb {b.z, b.w};
    const Point pc {c.z, c.w};
    const float first = cross(pb - pa, point - pa);
    const float second = cross(pc - pb, point - pb);
    const float third = cross(pa - pc, point - pc);
    return (first >= -kEpsilon && second >= -kEpsilon && third >= -kEpsilon) ||
           (first <= kEpsilon && second <= kEpsilon && third <= kEpsilon);
}

bool contains(const GrassCardVariant &variant, const Point &point) {
    for (size_t i = 0; i < variant.indices.size(); i += 3) {
        if (pointInTriangle(point,
                            variant.vertices[variant.indices[i]],
                            variant.vertices[variant.indices[i + 1]],
                            variant.vertices[variant.indices[i + 2]]))
            return true;
    }
    return false;
}

float area(const GrassCardVariant &variant) {
    float result = 0.0f;
    for (size_t i = 0; i < variant.indices.size(); i += 3) {
        const auto &a = variant.vertices[variant.indices[i]];
        const auto &b = variant.vertices[variant.indices[i + 1]];
        const auto &c = variant.vertices[variant.indices[i + 2]];
        result += std::abs(cross(Point {b.x - a.x, b.y - a.y}, Point {c.x - a.x, c.y - a.y})) * 0.5f;
    }
    return result;
}

GrassCardVariant makeObb(const VariantMask &mask) {
    const auto hull = texelHull(mask);
    float bestArea = std::numeric_limits<float>::max();
    Point bestMajor {1.0f, 0.0f};
    Point bestMinor {0.0f, 1.0f};
    Point bestMin {0.0f};
    Point bestMax {0.0f};
    for (size_t i = 0; i < hull.size(); ++i) {
        const Point edge = hull[(i + 1) % hull.size()] - hull[i];
        const Point major = edge / std::sqrt(glm::dot(edge, edge));
        const Point minor {-major.y, major.x};
        Point min {std::numeric_limits<float>::max()};
        Point max {std::numeric_limits<float>::lowest()};
        for (const auto &point : hull) {
            const Point projection {glm::dot(point, major), glm::dot(point, minor)};
            min = glm::min(min, projection);
            max = glm::max(max, projection);
        }
        const float candidateArea = (max.x - min.x) * (max.y - min.y);
        if (candidateArea < bestArea) {
            bestArea = candidateArea;
            bestMajor = major;
            bestMinor = minor;
            bestMin = min;
            bestMax = max;
        }
    }

    std::vector<Point> points;
    for (const Point &projection : std::array<Point, 4> {
             Point {bestMin.x, bestMin.y}, Point {bestMax.x, bestMin.y},
             Point {bestMax.x, bestMax.y}, Point {bestMin.x, bestMax.y}}) {
        points.push_back(bestMajor * projection.x + bestMinor * projection.y);
    }
    GrassCardVariant result;
    appendPolygon(result, points);
    return result;
}

GrassCardVariant makeKgon(const VariantMask &mask, int sides) {
    auto points = texelHull(mask);
    if (points.size() < 3)
        return makeQuad(mask.min, mask.max);

    while (points.size() > static_cast<size_t>(sides)) {
        float leastAddedArea = std::numeric_limits<float>::max();
        size_t best = points.size();
        Point replacement {0.0f};
        for (size_t b = 0; b < points.size(); ++b) {
            const size_t a = (b + points.size() - 1) % points.size();
            const size_t c = (b + 1) % points.size();
            const size_t d = (b + 2) % points.size();
            const Point firstEdge = points[b] - points[a];
            const Point secondEdge = points[d] - points[c];
            const float denominator = cross(firstEdge, secondEdge);
            if (std::abs(denominator) <= kEpsilon)
                continue;
            const float firstScale = cross(points[c] - points[a], secondEdge) / denominator;
            const Point intersection = points[a] + firstScale * firstEdge;
            const float addedArea = std::abs(cross(points[b] - intersection, points[c] - intersection)) * 0.5f;
            if (addedArea < leastAddedArea) {
                leastAddedArea = addedArea;
                best = b;
                replacement = intersection;
            }
        }
        if (best == points.size())
            return makeQuad(mask.min, mask.max);
        points[best] = replacement;
        points.erase(points.begin() + (best + 1) % points.size());
    }
    GrassCardVariant result;
    appendPolygon(result, points);
    return result;
}

GrassCardVariant makeBgrid(const VariantMask &mask, int grid) {
    const Point extent = mask.max - mask.min;
    std::vector<bool> kept(static_cast<size_t>(grid) * grid, false);
    for (const auto &centre : mask.centres) {
        const int x = std::min(grid - 1, static_cast<int>((centre.x - mask.min.x) / extent.x * grid));
        const int y = std::min(grid - 1, static_cast<int>((centre.y - mask.min.y) / extent.y * grid));
        kept[static_cast<size_t>(y) * grid + x] = true;
    }

    std::vector<bool> consumed(kept.size(), false);
    GrassCardVariant result;
    for (int y = 0; y < grid; ++y) {
        for (int x = 0; x < grid; ++x) {
            const size_t start = static_cast<size_t>(y) * grid + x;
            if (!kept[start] || consumed[start])
                continue;

            int right = x + 1;
            while (right < grid && kept[static_cast<size_t>(y) * grid + right] &&
                   !consumed[static_cast<size_t>(y) * grid + right])
                ++right;
            int bottom = y + 1;
            while (bottom < grid) {
                bool rowKept = true;
                for (int column = x; column < right; ++column) {
                    const size_t index = static_cast<size_t>(bottom) * grid + column;
                    if (!kept[index] || consumed[index]) {
                        rowKept = false;
                        break;
                    }
                }
                if (!rowKept)
                    break;
                ++bottom;
            }

            for (int row = y; row < bottom; ++row) {
                for (int column = x; column < right; ++column)
                    consumed[static_cast<size_t>(row) * grid + column] = true;
            }
            const Point min = mask.min + extent * Point {
                static_cast<float>(x) / grid, static_cast<float>(y) / grid};
            const Point max = mask.min + extent * Point {
                static_cast<float>(right) / grid, static_cast<float>(bottom) / grid};
            appendPolygon(result, {{min.x, min.y}, {max.x, min.y}, {max.x, max.y}, {min.x, max.y}});
        }
    }
    return result;
}

GrassCardAtlas makeQuadAtlas() {
    GrassCardAtlas result;
    for (auto &variant : result.variants)
        variant = makeQuad();
    result.vertsPerCard = 4;
    result.trisPerCard = 2;
    return result;
}

void pad(GrassCardAtlas &atlas) {
    for (const auto &variant : atlas.variants) {
        atlas.vertsPerCard = std::max(atlas.vertsPerCard, static_cast<uint32_t>(variant.vertices.size()));
        atlas.trisPerCard = std::max(atlas.trisPerCard, static_cast<uint32_t>(variant.indices.size() / 3));
    }
    for (auto &variant : atlas.variants) {
        const auto lastTriangle = std::array<uint32_t, 3> {
            variant.indices[variant.indices.size() - 3],
            variant.indices[variant.indices.size() - 2],
            variant.indices[variant.indices.size() - 1]};
        while (variant.indices.size() / 3 < atlas.trisPerCard)
            variant.indices.insert(variant.indices.end(), lastTriangle.begin(), lastTriangle.end());
        // Degenerate triangles make the tracer normalize a zero cross product;
        // coincident copies of a real triangle only add a harmless intersection.
        while (variant.vertices.size() < atlas.vertsPerCard)
            variant.vertices.push_back(variant.vertices.back());
    }
}

} // namespace

GrassCardAtlas fitGrassCards(const Texture &texture, const GrassCardParams &params) {
    const bool supportedAlpha = texture.pixelFormat() == PixelFormat::RGBA8 ||
                                texture.pixelFormat() == PixelFormat::BGRA8 ||
                                texture.pixelFormat() == PixelFormat::DXT5;
    if (!supportedAlpha || texture.layers().empty() ||
        !texture.layers().front().pixels || texture.width() < 2 || texture.height() < 2) {
        // DXT1's one-bit alpha is deliberately not fitted: it is not the soft
        // cutout the fitted outline is meant to remove, so retain the full card.
        return makeQuadAtlas();
    }

    const int width = texture.width();
    const int height = texture.height();
    std::vector<uint8_t> alpha(static_cast<size_t>(width) * height);
    const auto *pixels = reinterpret_cast<const uint8_t *>(texture.layers().front().pixels->data());
    switch (texture.pixelFormat()) {
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
        for (int i = 0; i < width * height; ++i)
            alpha[i] = pixels[4 * i + 3];
        break;
    case PixelFormat::DXT5: {
        std::vector<uint32_t> decoded(alpha.size());
        decompressDXT5(width, height, pixels, decoded.data());
        for (size_t i = 0; i < alpha.size(); ++i)
            alpha[i] = decoded[i] & 0xff;
        break;
    }
    default:
        return makeQuadAtlas();
    }

    GrassCardAtlas result;
    const float alphaTest = std::clamp(params.alphaTest, 0.0f, 1.0f);
    const int sides = std::clamp(params.sides, 3, 16);
    const int grid = std::clamp(params.grid, 2, 32);
    size_t opaqueCount = 0;
    size_t coveredCount = 0;
    float totalArea = 0.0f;
    for (int variantIndex = 0; variantIndex < 4; ++variantIndex) {
        const int x0 = (variantIndex % 2) * width / 2;
        const int x1 = (variantIndex % 2 + 1) * width / 2;
        const int y0 = (variantIndex / 2) * height / 2;
        const int y1 = (variantIndex / 2 + 1) * height / 2;
        VariantMask mask;
        mask.width = x1 - x0;
        mask.height = y1 - y0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (alpha[static_cast<size_t>(y) * width + x] / 255.0f < alphaTest)
                    continue;
                const Point centre {
                    (x - x0 + 0.5f) / mask.width,
                    (y - y0 + 0.5f) / mask.height};
                mask.centres.push_back(centre);
                mask.min = glm::min(mask.min, Point {
                    (x - x0) / static_cast<float>(mask.width),
                    (y - y0) / static_cast<float>(mask.height)});
                mask.max = glm::max(mask.max, Point {
                    (x - x0 + 1) / static_cast<float>(mask.width),
                    (y - y0 + 1) / static_cast<float>(mask.height)});
            }
        }

        auto &variant = result.variants[variantIndex];
        if (mask.centres.empty()) {
            // Zero-area fallback triangles poison geometric normals in the tracer.
            variant = makeQuad();
        } else {
            switch (params.shape) {
            case GrassCardShape::Quad:
                variant = makeQuad();
                break;
            case GrassCardShape::Aabb:
                variant = makeQuad(mask.min, mask.max);
                break;
            case GrassCardShape::Obb:
                variant = makeObb(mask);
                break;
            case GrassCardShape::Kgon:
                variant = makeKgon(mask, sides);
                break;
            case GrassCardShape::Bgrid:
                variant = makeBgrid(mask, grid);
                break;
            }
        }

        totalArea += area(variant);
        opaqueCount += mask.centres.size();
        for (const auto &centre : mask.centres)
            coveredCount += contains(variant, centre);
    }
    result.coverage = opaqueCount == 0 ? 1.0f : static_cast<float>(coveredCount) / opaqueCount;
    result.areaFraction = totalArea / 4.0f;
    pad(result);
    return result;
}

const char *grassCardShapeName(GrassCardShape shape) {
    switch (shape) {
    case GrassCardShape::Quad:
        return "quad";
    case GrassCardShape::Aabb:
        return "aabb";
    case GrassCardShape::Obb:
        return "obb";
    case GrassCardShape::Kgon:
        return "kgon";
    case GrassCardShape::Bgrid:
        return "bgrid";
    }
    throw std::invalid_argument("Unknown grass card shape");
}

GrassCardShape parseGrassCardShape(const std::string &value) {
    if (value == "quad")
        return GrassCardShape::Quad;
    if (value == "aabb")
        return GrassCardShape::Aabb;
    if (value == "obb")
        return GrassCardShape::Obb;
    if (value == "kgon")
        return GrassCardShape::Kgon;
    if (value == "bgrid")
        return GrassCardShape::Bgrid;
    throw std::invalid_argument("Unknown grass card shape: " + value);
}

} // namespace graphics

} // namespace reone
