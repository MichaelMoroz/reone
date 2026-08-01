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

#include "../pass.h"

namespace reone {

namespace graphics {

class VulkanRenderPass;

} // namespace graphics

namespace scene {

/**
 * Vulkan-free scene adapter for the graphics-side render-pass recorder.
 *
 * Scene pass selection and scene instance records stop here. The delegate owns
 * all native handles, descriptors, pipelines, uniform uploads and commands.
 */
class VulkanRenderPass : public IRenderPassExecutor {
public:
    explicit VulkanRenderPass(graphics::VulkanRenderPass &delegate) :
        _delegate(delegate) {
    }

    void beginPass(RenderPassName pass) override;

    void executeDraw(graphics::Mesh &mesh,
                     graphics::Material &material,
                     const glm::mat4 &transform,
                     const glm::mat4 &transformInv,
                     const glm::mat4 &prevTransform) override;

    void executeDrawSkinned(graphics::Mesh &mesh,
                            graphics::Material &material,
                            const glm::mat4 &transform,
                            const glm::mat4 &transformInv,
                            const glm::mat4 &prevTransform,
                            const std::vector<glm::mat4> &bones,
                            const std::vector<glm::mat4> &prevBones) override;

    void executeDrawDangly(graphics::Mesh &mesh,
                           graphics::Material &material,
                           const glm::mat4 &transform,
                           const glm::mat4 &transformInv,
                           const glm::mat4 &prevTransform,
                           const std::vector<glm::vec4> &positions) override;

    void executeDrawSaber(graphics::Mesh &mesh,
                          graphics::Material &material,
                          const glm::mat4 &transform,
                          const glm::mat4 &transformInv,
                          const glm::mat4 &prevTransform,
                          const glm::vec4 &displacement) override;

    void executeDrawBillboard(graphics::Texture &texture,
                              const glm::vec4 &color,
                              const glm::mat4 &transform,
                              const glm::mat4 &transformInv,
                              std::optional<float> size) override;

    void executeDrawParticles(graphics::Material &material,
                              const glm::ivec2 &gridSize,
                              const std::vector<ParticleInstance> &particles) override;

    void executeDrawGrass(float radius,
                          float quadSize,
                          graphics::Material &material,
                          const std::vector<GrassInstance> &instances) override;

private:
    graphics::VulkanRenderPass &_delegate;
};

} // namespace scene

} // namespace reone
