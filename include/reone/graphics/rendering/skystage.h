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
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "reone/graphics/types.h"
#include "reone/graphics/rhi/rhi.h"

namespace reone::graphics {

class ICommandBuffer;
class IImage;
class IRenderer;
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

/** Backend-free description of the room selected for the fixed sky bake. */
struct RayQuerySkyRoom {
    uint64_t identity {0};
    std::string name;
    glm::vec3 origin {0.0f};
    std::vector<RayQuerySkyMesh> meshes;
};

/** The cube a consumer samples the sky through for one frame. */
struct SkyBinding {
    const IImage *cube {nullptr};
    ImageView view;
    bool baked {false};
};

/**
 * The baked environment cube and the offline bake that fills it, shared by
 * every render mode that samples a sky. Owns nothing mode-specific: consumers
 * ask for a binding and bind it, and the sky is neutral rather than absent
 * whenever no bake is in use.
 */
class SkyStage : boost::noncopyable {
public:
    explicit SkyStage(IRenderer &renderer);
    ~SkyStage();

    void init();
    void deinit();

    /**
     * Renders the room's shell into the six cube faces, once per detected room.
     * Returns whether a bake is available for it.
     */
    bool bakeSkyRoom(ICommandBuffer &commandBuffer, const RayQuerySkyRoom &room);
    void clearSkyRoom();
    bool supportsSkyTexture(const Texture &texture) const;

    /**
     * The caller owns the per-frame decision rather than reading the latch,
     * because a room whose shell meshes have not activated yet must sample the
     * fallback for that frame even though the latch still holds a bake.
     */
    SkyBinding binding(bool useBaked) const;

private:
    IRenderer &_renderer;
    uint64_t _skyCubeRoom {0};
    bool _skyCubeReady {false};
    bool _inited {false};
    std::unique_ptr<IImage> _skyCube;
    std::array<std::unique_ptr<IImage>, kNumCubeFaces> _skyDepth;
    std::unique_ptr<IImage> _skyFallbackCube;
};

} // namespace reone::graphics
