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

#include "reone/scene/node/grass.h"

#include "reone/graphics/di/services.h"
#include "reone/graphics/material.h"
#include "reone/graphics/barycentricutil.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/model.h"
#include "reone/graphics/texture.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/textures.h"
#include "reone/scene/gpuscene.h"
#include "reone/scene/graph.h"
#include "reone/system/profiler.h"

using namespace reone::graphics;

namespace reone::scene {
namespace {

constexpr float kGrassDensityFactor = 0.5f;
constexpr uint32_t kGrassVariantCount = 4;

glm::vec3 transformPoint(const glm::mat4 &transform, const glm::vec3 &point) {
    return glm::vec3(transform * glm::vec4(point, 1.0f));
}

} // namespace

void GrassSceneNode::init() {
    setNameIds({0, _sceneGraph.internName(_aabbNode.name())});
    const auto mesh = _aabbNode.mesh()->mesh;
    const auto &faces = mesh->faces();
    for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
        const auto &face = faces[faceIndex];
        if (_properties.materials.count(face.material) == 0)
            continue;
        _grassFaces.push_back(static_cast<int>(faceIndex));
        if (!mesh->tryFaceUV2(face, glm::vec3(1.0f, 0.0f, 0.0f)))
            _hasLightmapUV = false;
    }
}

void GrassSceneNode::onAbsoluteTransformChanged() {
    _faceRecordsBuilt = false;
    _supportVerticesBuilt = false;
    _gpuSceneDirty = true;
}

void GrassSceneNode::rebuildSupportVertices() {
    R_PROFILE_ZONE("GrassSceneNode::support refresh");
    _supportFaces.clear();
    _supportVertices.clear();
    _supportVerticesBuilt = true;
    // A room gets one of these whether or not its walkmesh has a grass
    // material on any face, and everything below is driven off the faces that
    // do. Without this the empty case still walks the room's whole node tree
    // and transforms every triangle in it, to build a grid that then rejects
    // all of them against an empty bound.
    if (_grassFaces.empty())
        return;

    const auto mesh = _aabbNode.mesh()->mesh;
    const auto &faces = mesh->faces();
    const std::array<glm::vec3, 3> corners {
        glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)};
    _supportFaces.reserve(_grassFaces.size());
    _supportVertices.reserve(3 * _grassFaces.size());

    const auto emitWhole = [&]() {
        for (const auto faceIndex : _grassFaces) {
            const auto local = mesh->faceVertexCoords(faces[faceIndex]);
            _supportFaces.push_back({faceIndex, 1.0f, corners});
            for (const auto &point : local)
                _supportVertices.push_back(transformPoint(_absTransform, point));
        }
    };

    if (!_properties.groundModel) {
        emitWhole();
        return;
    }

    // Which surfaces to sample. The AABB node is the walkmesh itself, so
    // sampling it would just return where the root already is; background
    // scenery is the sky shell and never underfoot.
    std::vector<std::pair<const Mesh *, glm::mat4>> surfaces;
    std::vector<const ModelNode *> pending {_properties.groundModel->rootNode().get()};
    while (!pending.empty()) {
        const auto *node = pending.back();
        pending.pop_back();
        if (!node)
            continue;
        for (const auto &child : node->children())
            pending.push_back(child.get());
        if (node == &_aabbNode || node->isAABBMesh())
            continue;
        const auto triangles = node->mesh();
        if (!triangles || !triangles->mesh || !triangles->render || triangles->backgroundGeometry)
            continue;
        surfaces.emplace_back(triangles->mesh.get(), node->absoluteTransform());
    }
    if (surfaces.empty()) {
        emitWhole();
        return;
    }

    // Roots are already in world space; a node's absolute transform only
    // reaches model space. What closes the gap is where the room itself was
    // placed, which this node holds implicitly: its own transform is that
    // placement composed with the AABB node's.
    const glm::mat4 roomTransform = _absTransform * _aabbNode.absoluteTransformInverse();

    // World-space triangles, transformed once. The query is a point test in
    // plan, so a triangle standing on edge - a planter's own wall, a doorway
    // reveal - covers no ground and is dropped rather than tested per root.
    struct Support {
        glm::vec3 a, b, c;
        glm::vec2 min, max;
        // The plan-space edge basis and the reciprocal of its determinant.
        // Held rather than recomputed because the degeneracy test below already
        // solves for them, and the query would otherwise redo that solve - and
        // two divisions - for every candidate triangle it walks.
        glm::vec2 e0, e1;
        float invArea;
    };
    std::vector<Support> support;
    size_t surfaceTriangles = 0;
    for (const auto &[surface, nodeTransform] : surfaces)
        surfaceTriangles += surface->faces().size();
    support.reserve(surfaceTriangles);
    for (const auto &[surface, nodeTransform] : surfaces) {
        const glm::mat4 transform = roomTransform * nodeTransform;
        for (const auto &face : surface->faces()) {
            const auto local = surface->faceVertexCoords(face);
            Support triangle;
            triangle.a = transformPoint(transform, local[0]);
            triangle.b = transformPoint(transform, local[1]);
            triangle.c = transformPoint(transform, local[2]);
            triangle.e0 = glm::vec2(triangle.b) - glm::vec2(triangle.a);
            triangle.e1 = glm::vec2(triangle.c) - glm::vec2(triangle.a);
            const float area = triangle.e0.x * triangle.e1.y - triangle.e1.x * triangle.e0.y;
            if (std::fabs(area) < 1e-9f)
                continue;
            triangle.invArea = 1.0f / area;
            triangle.min = glm::min(glm::vec2(triangle.a),
                                    glm::min(glm::vec2(triangle.b), glm::vec2(triangle.c)));
            triangle.max = glm::max(glm::vec2(triangle.a),
                                    glm::max(glm::vec2(triangle.b), glm::vec2(triangle.c)));
            support.push_back(triangle);
        }
    }
    if (support.empty()) {
        emitWhole();
        return;
    }

    // A plan grid over the grass, not over the room. Grass covers a fraction
    // of a room and this is thrown away as soon as the records are built, so
    // the bound worth holding is the work per query: without it every query
    // walks every triangle in the room, which is a second of loading per room
    // rather than the millisecond it needs to be.
    glm::vec2 gridMin(std::numeric_limits<float>::max());
    glm::vec2 gridMax(std::numeric_limits<float>::lowest());
    for (const auto faceIndex : _grassFaces) {
        for (const auto &point : mesh->faceVertexCoords(faces[faceIndex])) {
            const glm::vec3 world = transformPoint(_absTransform, point);
            gridMin = glm::min(gridMin, glm::vec2(world));
            gridMax = glm::max(gridMax, glm::vec2(world));
        }
    }
    const glm::vec2 span = glm::max(gridMax - gridMin, glm::vec2(1e-3f));
    const int side = std::clamp(
        static_cast<int>(std::sqrt(static_cast<float>(support.size()) / 8.0f)), 1, 128);
    const glm::vec2 cell = span / static_cast<float>(side);
    std::vector<std::vector<uint32_t>> buckets(static_cast<size_t>(side) * side);
    const auto cellRange = [&](float value, float origin, float size) {
        return std::clamp(static_cast<int>((value - origin) / size), 0, side - 1);
    };
    for (uint32_t i = 0; i < support.size(); ++i) {
        const auto &triangle = support[i];
        if (triangle.max.x < gridMin.x || triangle.min.x > gridMax.x ||
            triangle.max.y < gridMin.y || triangle.min.y > gridMax.y)
            continue;
        const int x0 = cellRange(triangle.min.x, gridMin.x, cell.x);
        const int x1 = cellRange(triangle.max.x, gridMin.x, cell.x);
        const int y0 = cellRange(triangle.min.y, gridMin.y, cell.y);
        const int y1 = cellRange(triangle.max.y, gridMin.y, cell.y);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                buckets[static_cast<size_t>(y) * side + x].push_back(i);
    }

    // A correction is a height, so the search is vertical: the drawn surface
    // over the root's own ground position. Where several surfaces cover it -
    // a planter's soil and the paving that runs on underneath it - the one
    // nearest the walkmesh is the one the walkmesh was standing in for,
    // resolved upward on a tie because grass grows on top of what it meets.
    //
    // Bounded by the authored quad size. A root that would have to travel
    // further than a blade is tall has not found its own ground, it has found
    // a different storey, and leaving it where the walkmesh put it is the
    // lesser error.
    const float reach = std::max(_properties.quadSize, 1.0f);
    const auto onGround = [&](glm::vec3 root) {
        const int cx = cellRange(root.x, gridMin.x, cell.x);
        const int cy = cellRange(root.y, gridMin.y, cell.y);
        float best = root.z;
        float bestDistance = reach;
        for (const auto index : buckets[static_cast<size_t>(cy) * side + cx]) {
            const auto &triangle = support[index];
            if (root.x < triangle.min.x || root.x > triangle.max.x ||
                root.y < triangle.min.y || root.y > triangle.max.y)
                continue;
            const glm::vec2 d = glm::vec2(root) - glm::vec2(triangle.a);
            const float u = (d.x * triangle.e1.y - triangle.e1.x * d.y) * triangle.invArea;
            const float v = (triangle.e0.x * d.y - d.x * triangle.e0.y) * triangle.invArea;
            if (u < -1e-4f || v < -1e-4f || u + v > 1.0f + 1e-4f)
                continue;
            const float height = triangle.a.z + u * (triangle.b.z - triangle.a.z) +
                                 v * (triangle.c.z - triangle.a.z);
            const float distance = std::fabs(height - root.z);
            if (distance < bestDistance || (distance == bestDistance && height > best)) {
                bestDistance = distance;
                best = height;
            }
        }
        root.z = best;
        return root;
    };

    // Divide a face only where lifting its corners is not enough. The test is
    // the ground's own disagreement with the plane those corners span,
    // measured at the edge midpoints: flat ground agrees at once and is
    // emitted whole, a mound disagrees and is split until it does not. So the
    // meadow keeps its handful of large faces while a planter bed becomes the
    // few dozen that its soil actually needs.
    //
    // The tolerance is a tenth of the authored quad size because that is the
    // scale grass is judged at: an error much smaller than a blade is not
    // visible, and one approaching a blade hides it.
    const float tolerance = 0.1f * std::max(_properties.quadSize, 0.1f);
    const int maxDepth = 5;
    struct Pending {
        std::array<glm::vec3, 3> barycentric;
        std::array<glm::vec3, 3> world;
        int depth;
    };
    std::vector<Pending> stack;
    for (const auto faceIndex : _grassFaces) {
        const auto local = mesh->faceVertexCoords(faces[faceIndex]);
        const auto worldAt = [&](const glm::vec3 &b) {
            return onGround(transformPoint(
                _absTransform,
                graphics::barycentricToCartesian(local[0], local[1], local[2], b)));
        };
        stack.clear();
        stack.push_back({corners, {worldAt(corners[0]), worldAt(corners[1]), worldAt(corners[2])}, 0});
        while (!stack.empty()) {
            const Pending piece = stack.back();
            stack.pop_back();
            std::array<glm::vec3, 3> midBary;
            std::array<glm::vec3, 3> midWorld;
            bool split = false;
            if (piece.depth < maxDepth) {
                for (int edge = 0; edge < 3; ++edge) {
                    const int next = (edge + 1) % 3;
                    midBary[edge] = 0.5f * (piece.barycentric[edge] + piece.barycentric[next]);
                    midWorld[edge] = worldAt(midBary[edge]);
                    const float plane = 0.5f * (piece.world[edge].z + piece.world[next].z);
                    if (std::fabs(midWorld[edge].z - plane) > tolerance)
                        split = true;
                }
            }
            if (!split) {
                _supportFaces.push_back(
                    {faceIndex, 1.0f / static_cast<float>(1 << (2 * piece.depth)), piece.barycentric});
                for (const auto &point : piece.world)
                    _supportVertices.push_back(point);
                continue;
            }
            const int depth = piece.depth + 1;
            stack.push_back({{piece.barycentric[0], midBary[0], midBary[2]},
                             {piece.world[0], midWorld[0], midWorld[2]},
                             depth});
            stack.push_back({{midBary[0], piece.barycentric[1], midBary[1]},
                             {midWorld[0], piece.world[1], midWorld[1]},
                             depth});
            stack.push_back({{midBary[2], midBary[1], piece.barycentric[2]},
                             {midWorld[2], midWorld[1], piece.world[2]},
                             depth});
            stack.push_back({{midBary[0], midBary[1], midBary[2]},
                             {midWorld[0], midWorld[1], midWorld[2]},
                             depth});
        }
    }
}

void GrassSceneNode::rebuildFaceRecords() {
    R_PROFILE_ZONE("GrassSceneNode::face record refresh");
    _faceRecords.clear();

    if (!_supportVerticesBuilt)
        rebuildSupportVertices();
    _faceRecords.reserve(_supportFaces.size());

    const auto mesh = _aabbNode.mesh()->mesh;
    const auto &faces = mesh->faces();
    // Pieces of one parent face are contiguous, so the split's rounding error
    // is carried from each piece to the next and settled within the face.
    //
    // Rounding a piece on its own does not redistribute, which is what the
    // line below claims: a piece holds a 4^depth-th of its parent, so far
    // enough down the share falls under half a cluster and rounds away. How
    // far down depends on the density, so it is not a fixed depth. Worked
    // through for a face that wants 400 clusters: 384 survive at depth 3, 512
    // at depth 4 - more than the face had - and 0 at depth 5. So the uneven
    // ground this subdivision exists to follow was the ground losing its
    // grass, and the flat ground beside it keeping every blade.
    int carryFace = -1;
    float carry = 0.0f;
    for (size_t slot = 0; slot < _supportFaces.size(); ++slot) {
        const auto &support = _supportFaces[slot];
        const auto &face = faces[support.face];
        if (support.face != carryFace) {
            carryFace = support.face;
            carry = 0.0f;
        }
        // The parent's area times this piece's share of it, so dividing a face
        // redistributes its blades rather than multiplying them.
        const float wanted = getNumClustersInFace(face.area * support.areaFraction) + carry;
        const int clusterBudget = static_cast<int>(std::floor(wanted));
        carry = wanted - static_cast<float>(clusterBudget);
        if (clusterBudget <= 0)
            continue;

        const glm::vec3 v0 = _supportVertices[3 * slot + 0];
        const glm::vec3 v1 = _supportVertices[3 * slot + 1];
        const glm::vec3 v2 = _supportVertices[3 * slot + 2];
        glm::vec2 uv0 {0.0f}, uv1 {0.0f}, uv2 {0.0f};
        if (_hasLightmapUV) {
            uv0 = *mesh->tryFaceUV2(face, support.barycentric[0]);
            uv1 = *mesh->tryFaceUV2(face, support.barycentric[1]);
            uv2 = *mesh->tryFaceUV2(face, support.barycentric[2]);
        }

        GrassFace record;
        record.vertex0Uv0x = glm::vec4(v0, uv0.x);
        record.vertex1Uv0y = glm::vec4(v1, uv0.y);
        record.vertex2Uv1x = glm::vec4(v2, uv1.x);
        record.uv1yUv2QuadSize = glm::vec4(uv1.y, uv2, _properties.quadSize);
        record.probabilities = _properties.probabilities;
        record.boundsMin = glm::vec4(glm::min(v0, glm::min(v1, v2)), 0.0f);
        record.boundsMax = glm::vec4(glm::max(v0, glm::max(v1, v2)), 0.0f);
        // materialIndex is filled after material interning by GpuScene::prepare.
        // The scatter's seed, and the only thing the shader takes this for.
        // It has to be the piece rather than the walkmesh face it came from,
        // or every piece of a divided face scatters the same blades in the
        // same places and the division shows as a repeat.
        record.faceBudgetMaterialVariants = {
            static_cast<uint32_t>(slot),
            static_cast<uint32_t>(clusterBudget),
            0u,
            kGrassVariantCount};
        _faceRecords.push_back(record);
    }

    _faceRecordsBuilt = true;
    if (++_faceGeneration == 0)
        ++_faceGeneration;
}

void GrassSceneNode::update(float dt) {
    const bool enabled = _enabled && _sceneGraph.grassEnabled();
    if (!enabled) {
        if (_wasGrassEnabled)
            _sceneGraph.gpuScene().unregisterObject(id());
        _wasGrassEnabled = false;
        _gpuSceneDirty = true;
        return;
    }
    _wasGrassEnabled = true;

    if (!_faceRecordsBuilt || _grassGeneration != _sceneGraph.grassGeneration()) {
        _grassGeneration = _sceneGraph.grassGeneration();
        rebuildFaceRecords();
        _gpuSceneDirty = true;
    }
    (void)dt;
}

void GrassSceneNode::collectInto(GpuScene &scene) {
    if (!_enabled || !_sceneGraph.grassEnabled()) {
        scene.unregisterObject(id());
        return;
    }
    if (!_faceRecordsBuilt)
        rebuildFaceRecords();

    std::optional<std::reference_wrapper<Texture>> lightmap;
    if (_hasLightmapUV && !_aabbNode.mesh()->lightmap.empty()) {
        lightmap = *_resourceSvc.textures.get(
            _aabbNode.mesh()->lightmap, TextureUsage::Lightmap);
    }
    Material material;
    material.type = MaterialType::Grass;
    material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)] =
        _properties.texture;
    if (lightmap) {
        material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)] =
            &lightmap->get();
    }
    material.faceCulling = FaceCullMode::None;
    material.alphaTest = _properties.alphaTest;
    scene.addGrass(renderCategory(RenderCategory::Opaque), id(), nameIds(), material,
                   _faceRecords, _faceGeneration);
    _gpuSceneDirty = false;
}

float GrassSceneNode::getNumClustersInFace(float area) const {
    // Budgets bake at the density CAP, not the live dial: the merge kernel
    // gates the active prefix by density/cap from a push constant, so the
    // slider is live without rebuilding face records. The cap matches the
    // editor slider's maximum.
    //
    // Unrounded, because a subdivided face asks this once per piece and the
    // rounding has to happen across the set rather than inside it. See
    // rebuildFaceRecords.
    return kGrassDensityFactor * kGrassDensityCap * _properties.density * area;
}

} // namespace reone::scene
