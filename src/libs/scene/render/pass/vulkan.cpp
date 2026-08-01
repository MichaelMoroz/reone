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

#include "reone/scene/render/pass/vulkan.h"

#include "reone/graphics/vulkan/renderpass.h"

namespace reone::scene {

void VulkanRenderPass::beginPass(RenderPassName pass) {
    using ShadowKind = graphics::VulkanRenderPass::ShadowKind;
    switch (pass) {
    case RenderPassName::DirLightShadowsPass:
        _delegate.setShadowKind(ShadowKind::Directional);
        break;
    case RenderPassName::PointLightShadows:
        _delegate.setShadowKind(ShadowKind::Point);
        break;
    default:
        _delegate.setShadowKind(ShadowKind::None);
        break;
    }
}

void VulkanRenderPass::executeDraw(graphics::Mesh &mesh,
                                   graphics::Material &material,
                                   const glm::mat4 &transform,
                                   const glm::mat4 &transformInv,
                                   const glm::mat4 &prevTransform) {
    _delegate.executeDraw(mesh, material, transform, transformInv, prevTransform);
}

void VulkanRenderPass::executeDrawSkinned(graphics::Mesh &mesh,
                                          graphics::Material &material,
                                          const glm::mat4 &transform,
                                          const glm::mat4 &transformInv,
                                          const glm::mat4 &prevTransform,
                                          const std::vector<glm::mat4> &bones,
                                          const std::vector<glm::mat4> &prevBones) {
    _delegate.executeDrawSkinned(mesh, material, transform, transformInv, prevTransform,
                                 bones, prevBones);
}

void VulkanRenderPass::executeDrawDangly(graphics::Mesh &mesh,
                                         graphics::Material &material,
                                         const glm::mat4 &transform,
                                         const glm::mat4 &transformInv,
                                         const glm::mat4 &prevTransform,
                                         const std::vector<glm::vec4> &positions) {
    _delegate.executeDrawDangly(mesh, material, transform, transformInv, prevTransform, positions);
}

void VulkanRenderPass::executeDrawSaber(graphics::Mesh &mesh,
                                        graphics::Material &material,
                                        const glm::mat4 &transform,
                                        const glm::mat4 &transformInv,
                                        const glm::mat4 &prevTransform,
                                        const glm::vec4 &displacement) {
    _delegate.executeDrawSaber(mesh, material, transform, transformInv, prevTransform, displacement);
}

void VulkanRenderPass::executeDrawBillboard(graphics::Texture &texture,
                                            const glm::vec4 &color,
                                            const glm::mat4 &transform,
                                            const glm::mat4 &transformInv,
                                            std::optional<float> size) {
    _delegate.executeDrawBillboard(texture, color, transform, transformInv, size);
}

void VulkanRenderPass::executeDrawParticles(graphics::Material &material,
                                            const glm::ivec2 &gridSize,
                                            const std::vector<ParticleInstance> &particles) {
    std::vector<graphics::VulkanParticleInstance> lowered;
    lowered.reserve(particles.size());
    for (const auto &particle : particles) {
        lowered.push_back({particle.frame, particle.position, particle.size, particle.color,
                           particle.right, particle.up});
    }
    _delegate.executeDrawParticles(material, gridSize, lowered);
}

void VulkanRenderPass::executeDrawGrass(float radius,
                                        float quadSize,
                                        graphics::Material &material,
                                        const std::vector<GrassInstance> &instances) {
    std::vector<graphics::VulkanGrassInstance> lowered;
    lowered.reserve(instances.size());
    for (const auto &instance : instances) {
        lowered.push_back(
            {instance.variant, instance.position, instance.lightmapUV, instance.yaw});
    }
    _delegate.executeDrawGrass(radius, quadSize, material, lowered);
}

} // namespace reone::scene
