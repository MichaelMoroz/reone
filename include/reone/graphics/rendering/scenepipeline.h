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

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/rendering/gbuffer.h"
#include "reone/graphics/rhi/commandbuffer.h"
#include "reone/graphics/rendering/gpuscene.h"
#include "reone/graphics/rhi/renderer.h"

namespace reone::graphics {

class IMeshRegistry;
class Uniforms;
class TextureRegistry;
struct GraphicsOptions;

enum class SceneStep {
    ProcessPBRTextures,
    Shadow,
    Geometry,
    PBRResolve,
    RetroResolve,
    Blended,
};

enum class SceneShadow {
    None,
    Directional,
    Point,
};

struct SceneFramePlan {
    SceneShadow shadow {SceneShadow::None};
    std::vector<SceneStep> steps;
};

struct PrimaryRayContext {
    ICommandBuffer *commandBuffer {nullptr};
    uint32_t globalsOffset {0};
    IImage *output {nullptr};
    glm::mat4 view {1.0f};
    glm::mat4 projection {1.0f};
    glm::vec4 jitter {0.0f};
};

struct ExternalTarget {
    const char *name {nullptr};
    const char *dumpName {nullptr};
    IImage *image {nullptr};
};

class ISceneCallbacks {
public:
    virtual ~ISceneCallbacks() = default;
    virtual void renderPrimary(const PrimaryRayContext &context) = 0;
    virtual GpuScene::View mergeGeometry(ICommandBuffer &commandBuffer) = 0;
    virtual std::vector<ExternalTarget> primaryTargets() const = 0;
};

enum class TargetKind { Color,
                              Depth,
                              EyeNormal,
                              Motion };
struct TargetInfo {
    std::string name;
    TargetKind kind {TargetKind::Color};
};

/** Owns native render targets, descriptors, barriers, pipelines and commands. */
class ScenePipeline : boost::noncopyable {
public:
    ScenePipeline(glm::ivec2 targetSize,
                        GraphicsOptions &options,
                        IRenderer &renderer,
                        Uniforms &uniforms,
                        IMeshRegistry &meshRegistry,
                        TextureRegistry &textureRegistry,
                        bool primaryRayMode);
    ~ScenePipeline();

    void init();
    void deinit();
    Texture &render(const SceneFramePlan &plan, ISceneCallbacks &callbacks);
    std::vector<TargetInfo> targets(const ISceneCallbacks &callbacks) const;
    void *renderTargetPreview(const std::string &name, int mode, float scale,
                              const ISceneCallbacks &callbacks);
    void dumpTargets(const std::filesystem::path &dir,
                     const ISceneCallbacks &callbacks);

private:
    glm::ivec2 _targetSize;
    GraphicsOptions &_options;
    IRenderer &_renderer;
    Uniforms &_uniforms;
    IMeshRegistry &_meshRegistry;
    TextureRegistry &_textureRegistry;
    bool _inited {false};
    bool _primaryRayMode {false};
    SceneShadow _shadow {SceneShadow::None};

    std::unique_ptr<GBuffer> _gbuffer;
    std::unique_ptr<IImage> _output;
    std::unique_ptr<IImage> _dirShadows;
    std::unique_ptr<IImage> _pointShadows;
    std::shared_ptr<Texture> _outputHandle;
    DescriptorSet _retroResolveSet;
    DescriptorSet _pbrResolveSet;
    DescriptorSet _resolveMaterialSet;
    GpuScene::View _mergedScene;
    bool _mergedScenePrepared {false};

    struct Preview {
        std::unique_ptr<IImage> image;
        void *imguiTexture {nullptr};
        std::string target;
        int mode {0};
        float scale {1.0f};
    };
    std::unique_ptr<Preview> _preview;

    struct Target {
        const char *name;
        const char *dumpName;
        TargetKind kind;
        const IImage *image;
        ImageLayout layout;
        bool depth;
    };

    const GpuScene::View &prepareMergedScene(
        ICommandBuffer &cmd, ISceneCallbacks &callbacks);
    void shadowPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                    ISceneCallbacks &callbacks);
    void previewPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                     const ISceneCallbacks &callbacks);
    void geometryPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                      ISceneCallbacks &callbacks);
    void retroResolvePass(ICommandBuffer &cmd, uint32_t globalsOffset);
    /** G8: the transparent surfaces the G-buffer deliberately leaves out,
        drawn forward onto the resolved image in submission order. */
    void blendedPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                     ISceneCallbacks &callbacks);
    void pbrResolvePass(ICommandBuffer &cmd, uint32_t globalsOffset);
    std::vector<Target> targetEntries(const ISceneCallbacks &callbacks) const;
};

} // namespace reone::graphics
