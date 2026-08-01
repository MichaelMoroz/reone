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

#include "reone/graphics/types.h"

#include "../gpuscene.h"

namespace reone {

namespace graphics {

class IContext;
class IMeshRegistry;
class IPBRTextures;
class IStatistic;
class ITextureRegistry;
class IUniforms;
class Mesh;
class Texture;

struct GraphicsOptions;
struct LocalUniforms;
struct Material;

} // namespace graphics

namespace scene {

/**
 * Executes entries selected from a completed frame registry.
 *
 * Scene code never sees this interface. Keeping execution separate from
 * registration prevents a pass from causing another scene traversal.
 */
class IRenderPassExecutor {
public:
    virtual ~IRenderPassExecutor() = default;

    /** Called once before this executor walks the entries selected for a pass. */
    virtual void beginPass(RenderPassName pass) = 0;

    virtual void executeDraw(graphics::Mesh &mesh,
                             graphics::Material &material,
                             const glm::mat4 &transform,
                             const glm::mat4 &transformInv,
                             const glm::mat4 &prevTransform) = 0;
    virtual void executeDrawSkinned(graphics::Mesh &mesh,
                                    graphics::Material &material,
                                    const glm::mat4 &transform,
                                    const glm::mat4 &transformInv,
                                    const glm::mat4 &prevTransform,
                                    const std::vector<glm::mat4> &bones,
                                    const std::vector<glm::mat4> &prevBones) = 0;
    virtual void executeDrawDangly(graphics::Mesh &mesh,
                                   graphics::Material &material,
                                   const glm::mat4 &transform,
                                   const glm::mat4 &transformInv,
                                   const glm::mat4 &prevTransform,
                                   const std::vector<glm::vec4> &positions) = 0;
    virtual void executeDrawSaber(graphics::Mesh &mesh,
                                  graphics::Material &material,
                                  const glm::mat4 &transform,
                                  const glm::mat4 &transformInv,
                                  const glm::mat4 &prevTransform,
                                  const glm::vec4 &displacement) = 0;
    virtual void executeDrawBillboard(graphics::Texture &texture,
                                      const glm::vec4 &color,
                                      const glm::mat4 &transform,
                                      const glm::mat4 &transformInv,
                                      std::optional<float> size) = 0;
    virtual void executeDrawParticles(graphics::Material &material,
                                      const glm::ivec2 &gridSize,
                                      const std::vector<ParticleInstance> &particles) = 0;
    virtual void executeDrawGrass(float radius,
                                  float quadSize,
                                  graphics::Material &material,
                                  const std::vector<GrassInstance> &instances) = 0;
};

} // namespace scene

} // namespace reone
