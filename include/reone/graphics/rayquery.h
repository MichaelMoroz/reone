/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "reone/graphics/gpuscene.h"

namespace reone::graphics {

class Mesh;
class Texture;

struct RayQuerySkyMesh {
    const Mesh *mesh {nullptr};
    const Texture *texture {nullptr};
    glm::mat4 transform {1.0f};
    glm::mat4 transformInv {1.0f};
    glm::mat4 prevTransform {1.0f};
    glm::mat3x4 uv {1.0f};
};

/** Vulkan-free description of the room selected for the fixed sky bake. */
struct RayQuerySkyRoom {
    uint64_t identity {0};
    std::string name;
    glm::vec3 origin {0.0f};
    std::vector<RayQuerySkyMesh> meshes;
};

/** What the scene side hands the tracer for this frame: the upload and its counts. */
struct RayQuerySubmission {
    GpuSceneUpload upload;
    uint32_t dynamicTriangles {0};
    uint32_t skinned {0};
    uint32_t deforming {0};
    uint32_t outOfRange {0};
    uint32_t emissive {0};
    uint32_t additive {0};
    uint32_t sabers {0};
    uint32_t sky {0};
    uint32_t dangly {0};
    uint32_t grass {0};
    uint32_t particles {0};
    uint32_t billboards {0};
};

} // namespace reone::graphics
