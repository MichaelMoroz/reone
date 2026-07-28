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

#include "../pass.h"

namespace reone {

namespace graphics {

class ShaderProgram;

}

namespace scene {

class PBRRenderPass : public IRenderPassExecutor {
public:
    PBRRenderPass(graphics::GraphicsOptions &options,
                  graphics::IContext &context,
                  graphics::IShaderRegistry &shaderRegistry,
                  graphics::IStatistic &statistic,
                  graphics::IMeshRegistry &meshRegistry,
                  graphics::IPBRTextures &pbrTextures,
                  graphics::ITextureRegistry &textureRegistry,
                  graphics::IUniforms &uniforms) :
        _options(options),
        _context(context),
        _shaderRegistry(shaderRegistry),
        _statistic(statistic),
        _meshRegistry(meshRegistry),
        _pbrTextures(pbrTextures),
        _textureRegistry(textureRegistry),
        _uniforms(uniforms) {
    }

    void beginPass(RenderPassName pass) override { _pass = pass; }

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

    void executeDrawAABB(const std::vector<glm::vec4> &corners) override;
    void executeDrawDebug(const std::function<void()> &execute) override {
        execute();
    }


private:
    graphics::GraphicsOptions &_options;
    graphics::IContext &_context;
    graphics::IShaderRegistry &_shaderRegistry;
    graphics::IStatistic &_statistic;
    graphics::IMeshRegistry &_meshRegistry;
    graphics::IPBRTextures &_pbrTextures;
    graphics::ITextureRegistry &_textureRegistry;
    graphics::IUniforms &_uniforms;

    /** Resolved per material, then written into the Locals block. */
    int _envMapDerivedLayer {0};

    /**
     * Which vertex path a draw takes. The rewritten shaders specialise on this
     * rather than branching on a feature flag, so it selects the program.
     */
    enum class GeometryPath {
        Static,
        Skinned,
        Dangly,
        Saber
    };

    GeometryPath _geometryPath {GeometryPath::Static};
    RenderPassName _pass {RenderPassName::None};

    bool isShadowPass() const;
    void applyMaterialToLocals(const graphics::Material &material, graphics::LocalUniforms &locals);

    void withMaterialAppliedToContext(const graphics::Material &material, std::function<void(graphics::ShaderProgram &)> block);
};

} // namespace scene

} // namespace reone
