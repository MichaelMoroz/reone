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

#include "reone/graphics/framebuffer.h"
#include "reone/graphics/renderbuffer.h"
#include "reone/graphics/texture.h"

#include "pass.h"

template <>
struct std::hash<glm::ivec2> {
    size_t operator()(const glm::ivec2 &dim) const {
        size_t hash = 0;
        boost::hash_combine(hash, dim.x);
        boost::hash_combine(hash, dim.y);
        return hash;
    }
};

namespace reone {

namespace graphics {

class IStatistic;

class Context;
class MeshRegistry;
class PBRTextures;
class ShaderRegistry;
class TextureRegistry;
class Uniforms;
class VulkanRenderer;

struct GraphicsOptions;

} // namespace graphics

namespace scene {

enum class RendererType {
    Retro,
    PBR,
    Vulkan
};

/**
 * How a render target should be interpreted when displayed. Several targets hold
 * values that are not directly viewable - depth is non-linear, normals are
 * biased into unit range, motion vectors are small and signed.
 */
enum class RenderTargetKind {
    Color,
    Depth,
    EyeNormal,
    Motion
};

struct RenderTargetInfo {
    std::string name;
    RenderTargetKind kind {RenderTargetKind::Color};
    graphics::Texture *texture {nullptr};
};

class IRenderPipeline {
public:
    virtual ~IRenderPipeline() = default;

    virtual void init() = 0;

    virtual void reset() = 0;
    virtual void inRenderPass(RenderPassName name, std::function<void(IRenderPass &)> block) = 0;

    virtual graphics::Texture &render() = 0;

    /**
     * Intermediate targets, for inspection by development tooling. Empty unless
     * the pipeline chooses to expose any.
     */
    virtual std::vector<RenderTargetInfo> targets() const = 0;
};

class IRenderPipelineFactory {
public:
    virtual ~IRenderPipelineFactory() = default;

    virtual std::unique_ptr<IRenderPipeline> create(RendererType type, glm::ivec2 targetSize) = 0;

    /**
     * Hand the factory the Vulkan renderer, so it can build a Vulkan pipeline.
     * The scene library cannot reach it otherwise: the engine owns it.
     */
    virtual void setVulkanRenderer(graphics::VulkanRenderer &renderer) = 0;
};

class RenderPipelineBase : public IRenderPipeline, boost::noncopyable {
public:
    using RenderPassCallback = std::function<void(IRenderPass &)>;

    void reset() override {
        _passCallbacks.clear();
    }

    void inRenderPass(RenderPassName name, RenderPassCallback callback) override {
        _passCallbacks[name] = std::move(callback);
    }

    std::vector<RenderTargetInfo> targets() const override {
        return {};
    }

protected:
    struct GaussianBlurParams {
        bool vertical {false};
        bool strong {false};
    };

    glm::ivec2 _targetSize;
    graphics::GraphicsOptions &_options;
    graphics::Context &_context;
    graphics::MeshRegistry &_meshRegistry;
    graphics::ShaderRegistry &_shaderRegistry;
    graphics::IStatistic &_statistic;
    graphics::TextureRegistry &_textureRegistry;
    graphics::Uniforms &_uniforms;

    bool _inited {false};

    glm::mat4 _shadowLightSpace[graphics::kNumShadowLightSpace] {glm::mat4(1.0f)};
    glm::vec4 _shadowCascadeFarPlanes {glm::vec4(0.0f)};

    RenderPassName _passName {RenderPassName::None};
    std::map<RenderPassName, RenderPassCallback> _passCallbacks;

    RenderPipelineBase(glm::ivec2 targetSize,
                       graphics::GraphicsOptions &options,
                       graphics::Context &context,
                       graphics::MeshRegistry &meshRegistry,
                       graphics::ShaderRegistry &shaderRegistry,
                       graphics::IStatistic &statistic,
                       graphics::TextureRegistry &textureRegistry,
                       graphics::Uniforms &uniforms) :
        _targetSize(std::move(targetSize)),
        _options(options),
        _context(context),
        _meshRegistry(meshRegistry),
        _shaderRegistry(shaderRegistry),
        _statistic(statistic),
        _textureRegistry(textureRegistry),
        _uniforms(uniforms) {
    }

    void applyBoxBlur(graphics::Texture &tex, graphics::Framebuffer &dst, const glm::ivec2 &size);
    void applyGaussianBlur(graphics::Texture &tex, graphics::Framebuffer &dst, const glm::ivec2 &size, const GaussianBlurParams &params);
    void applyMedianFilter(graphics::Texture &tex, graphics::Framebuffer &dst, const glm::ivec2 &size, bool strong = false);
    void applyFXAA(graphics::Texture &tex, graphics::Framebuffer &dst, const glm::ivec2 &size);
    void applySharpen(graphics::Texture &tex, graphics::Framebuffer &dst, const glm::ivec2 &size, float amount);
};

class RenderPipelineFactory : public IRenderPipelineFactory, boost::noncopyable {
public:
    RenderPipelineFactory(graphics::GraphicsOptions &options,
                          graphics::Context &context,
                          graphics::MeshRegistry &meshRegistry,
                          graphics::PBRTextures &pbrTextures,
                          graphics::ShaderRegistry &shaderRegistry,
                          graphics::IStatistic &statistic,
                          graphics::TextureRegistry &textureRegistry,
                          graphics::Uniforms &uniforms) :
        _options(options),
        _context(context),
        _meshRegistry(meshRegistry),
        _pbrTextures(pbrTextures),
        _shaderRegistry(shaderRegistry),
        _statistic(statistic),
        _textureRegistry(textureRegistry),
        _uniforms(uniforms) {
    }

    std::unique_ptr<IRenderPipeline> create(RendererType type, glm::ivec2 targetSize) override;

    void setVulkanRenderer(graphics::VulkanRenderer &renderer) override {
        _vulkanRenderer = &renderer;
    }

private:
    graphics::GraphicsOptions &_options;
    graphics::Context &_context;
    graphics::MeshRegistry &_meshRegistry;
    graphics::PBRTextures &_pbrTextures;
    graphics::ShaderRegistry &_shaderRegistry;
    graphics::IStatistic &_statistic;
    graphics::TextureRegistry &_textureRegistry;
    graphics::Uniforms &_uniforms;
    graphics::VulkanRenderer *_vulkanRenderer {nullptr};
};

} // namespace scene

} // namespace reone
