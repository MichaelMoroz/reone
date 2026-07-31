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

#include "reone/graphics/texture.h"

#include "../registry.h"
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
class TextureRegistry;
class Uniforms;
class VulkanRenderer;

struct GraphicsOptions;

} // namespace graphics

namespace scene {

class CameraSceneNode;

/**
 * Which renderer to run.
 */
enum class RenderMode {
    Retro,
    PBR,
    /** Vulkan-only primary-ray view. Kept here for the future tracer. */
    PathTracing
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

    virtual graphics::Texture &render(RenderRegistry &registry,
                                      const CameraSceneNode *camera,
                                      RenderPassName shadowPass,
                                      const graphics::Frustum *shadowFrusta,
                                      size_t numShadowFrusta) = 0;

    /**
     * Intermediate targets, for inspection by development tooling. Empty unless
     * the pipeline chooses to expose any.
     */
    virtual std::vector<RenderTargetInfo> targets() const = 0;

    virtual void *renderTargetPreview(const std::string &name, int mode, float scale) {
        return nullptr;
    }

    /**
     * Write every exposed target into @p dir, one .npy per target.
     *
     * For comparing one backend against another: what a screenshot shows is the
     * end of a long chain, and when two backends disagree it says nothing about
     * where. Dumping the G-buffer separates "the geometry pass wrote different
     * values" from "the resolve read them differently".
     *
     * Read back exactly as stored, so a 32-bit depth target arrives as 32-bit
     * floats rather than being flattened into something displayable. The GPU
     * has to be idle before this is called; the caller owns that.
     */
    virtual void dumpTargets(const std::filesystem::path &dir) = 0;

    /**
     * Throw away every temporal history the pipeline holds, so the next frame
     * accumulates from nothing.
     *
     * A temporal filter with a blend factor never reaches zero residual - it
     * settles at a small steady state - so a measurement that only sees the
     * settled value cannot tell a working filter from a broken one. Restarting
     * the history at a known frame makes the approach itself observable: the
     * residual must fall geometrically from its cold value to that steady
     * state, and a filter that is not accumulating shows no decay at all.
     */
    virtual void restartTemporalHistory() {}
};

class IRenderPipelineFactory {
public:
    virtual ~IRenderPipelineFactory() = default;

    virtual std::unique_ptr<IRenderPipeline> create(RenderMode mode, glm::ivec2 targetSize) = 0;

    /**
     * Hand the factory the Vulkan renderer, so it can build a Vulkan pipeline.
     * The scene library cannot reach it otherwise: the engine owns it.
     */
    virtual void setVulkanRenderer(graphics::VulkanRenderer &renderer) = 0;
};

class RenderPipelineFactory : public IRenderPipelineFactory, boost::noncopyable {
public:
    RenderPipelineFactory(graphics::GraphicsOptions &options,
                          graphics::MeshRegistry &meshRegistry,
                          graphics::TextureRegistry &textureRegistry,
                          graphics::Uniforms &uniforms) :
        _options(options),
        _meshRegistry(meshRegistry),
        _textureRegistry(textureRegistry),
        _uniforms(uniforms) {
    }

    std::unique_ptr<IRenderPipeline> create(RenderMode mode, glm::ivec2 targetSize) override;

    void setVulkanRenderer(graphics::VulkanRenderer &renderer) override {
        _vulkanRenderer = &renderer;
    }

private:
    graphics::GraphicsOptions &_options;
    graphics::MeshRegistry &_meshRegistry;
    graphics::TextureRegistry &_textureRegistry;
    graphics::Uniforms &_uniforms;
    graphics::VulkanRenderer *_vulkanRenderer {nullptr};
};

} // namespace scene

} // namespace reone
