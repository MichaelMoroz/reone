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

#include "reone/scene/node/walkmesh.h"

#include "reone/graphics/types.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

const Mesh *WalkmeshSceneNode::debugMesh() {
    if (_debugMeshBuilt) {
        return _debugMesh.get();
    }
    _debugMeshBuilt = true;

    const size_t faceCount = _walkmesh.faces.size();
    if (faceCount == 0) {
        return nullptr;
    }
    std::vector<float> vertices;
    std::vector<Mesh::Face> faces;
    vertices.reserve(faceCount * 3 * 7);
    faces.reserve(faceCount);

    for (uint32_t i = 0; i < faceCount; ++i) {
        const auto wface = _walkmesh.getFace(i);
        const size_t vertIdxStart = vertices.size() / 7;
        // Normalised, because the shader scales back up by the same constant:
        // trigger geometry carries a flat 1.0 to reach the last slot, and that
        // only lines up if every surface id travels the same way.
        const float material = glm::min(
            1.0f, static_cast<float>(wface.material) /
                      static_cast<float>(kMaxWalkmeshMaterials - 1));

        for (const glm::vec3 &v : wface.vertices) {
            vertices.push_back(v.x);
            vertices.push_back(v.y);
            vertices.push_back(v.z);
            vertices.push_back(wface.normal.x);
            vertices.push_back(wface.normal.y);
            vertices.push_back(wface.normal.z);
            vertices.push_back(material);
        }

        Mesh::Face face;
        face.vertices[0] = static_cast<uint32_t>(vertIdxStart);
        face.vertices[1] = static_cast<uint32_t>(vertIdxStart + 1);
        face.vertices[2] = static_cast<uint32_t>(vertIdxStart + 2);
        face.normal = wface.normal;
        face.material = wface.material;
        faces.push_back(std::move(face));
    }

    Mesh::VertexLayout vertexLayout;
    vertexLayout.stride = 7 * sizeof(float);
    vertexLayout.offPosition = 0;
    vertexLayout.offNormals = 3 * sizeof(float);
    vertexLayout.offMaterial = 6 * sizeof(float);

    _debugMesh = std::make_unique<Mesh>(
        std::move(vertices), std::move(vertexLayout), std::move(faces));
    return _debugMesh.get();
}

} // namespace scene

} // namespace reone
