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
#include "reone/graphics/rendering/sky.h"
// GBufferBinding: the rasterized primary the tracer is handed.
#include "reone/graphics/rhi/pipelinecache.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/graphics/rhi/upscaler.h"

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
    /**
     * Screen-space reflections, over the image the PBR resolve produced.
     *
     * A step of its own rather than part of the resolve because a reflection
     * lands on some other pixel: the dispatch has to read a finished image, and
     * the resolve's own dispatch has not finished writing one. It sits ahead of
     * the anti-aliasing slot because reflections belong to the opaque image a
     * temporal resolve resolves, and ahead of transparency because nothing
     * transparent is in the buffer it marches through.
     *
     * Appended only when the option is on, so off costs nothing at all.
     */
    ScreenSpaceReflections,
    Blended,
    /** The common tail, in this order and in every mode: the anti-aliasing
        slot resolves the opaque image, then transparency is drawn over the
        result, then the display transform closes it. Both tail passes
        ping-pong onto the tail target, so a step that does not run costs
        nothing rather than a copy.

        Transparency sits after the resolve because a temporal one cannot
        reproject it: billboards write no motion, so anything drawn before the
        resolve ghosts behind the camera. */
    AntiAliasing,
    /** After the display transform: an unsharp mask over display colour. */
    Sharpen,
    PostProcess,
    /**
     * A debug channel view, over the shaded image, in any render mode.
     *
     * Last and alone: it overwrites every pixel with a diagnostic rather than a
     * picture, and the plan omits the anti-aliasing, grade and sharpen steps
     * whenever it is present - filtering or tone-mapping a channel would change
     * the values the view exists to show. Not appended in the traced mode when
     * the selected channel is one of the tracer's own, because the kernel has
     * already written that channel itself.
     */
    DebugView,
};

/**
 * The debug channels that exist only inside the path tracer.
 *
 * Its demodulated diffuse and specular radiance and its noise-free channel are
 * the kernel's own intermediates; no raster mode has anything to show for them,
 * and substituting a different channel would make the diagnostic lie. The
 * traced mode writes them from the kernel and skips the shared debug pass; the
 * other modes run the pass, which paints an explicit "not available" card.
 * Mirrors the kDebug* numbering in slang/debug_view.slang.
 */
inline bool isTracedOnlyDebugView(int view) {
    // Must agree with isTracedOnlyDebugView in slang/debug_view.slang: this
    // decides whether the shared debug pass steps aside, and that one decides
    // whether it paints the not-available card. Disagreeing means a channel
    // that exists is covered over by the card that says it does not.
    return view == 8 || view == 9 || view == 11 || (view >= 15 && view <= 19);
}

enum class SceneShadow {
    None,
    Directional,
    Point,
};

/** Every object category casts; see SceneFramePlan::shadowCasterCategories. */
constexpr uint32_t kAllShadowCasters = 0xFFFFFFFFu;

struct SceneFramePlan {
    SceneShadow shadow {SceneShadow::None};
    /**
     * Bit per scene::ModelUsage allowed into the shadow map.
     *
     * The policy is the scene layer's - it is the only one that knows what a
     * category means - and the shadow pass only applies it. Retro admits
     * characters alone, which is both what the original drew and what keeps
     * terrain from self-shadowing; PBR admits everything.
     */
    uint32_t shadowCasterCategories {kAllShadowCasters};
    std::vector<SceneStep> steps;
};

struct PrimaryRayContext {
    ICommandBuffer *commandBuffer {nullptr};
    uint32_t globalsOffset {0};
    IImage *output {nullptr};
    GpuScene::View scene;
    glm::mat4 view {1.0f};
    glm::mat4 projection {1.0f};
    glm::vec4 jitter {0.0f};
    /** The primary the geometry pass just rasterized; the tracer starts here. */
    GBufferBinding gbuffer;
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
    /**
     * This frame's sky cube, baking the detected room first if it changed.
     *
     * The bake needs the scene's registered meshes, so the decision of what to
     * bake stays scene-side and only the resulting cube crosses over. It
     * records into @p commandBuffer, so it must be called outside a pass.
     */
    virtual SkyBinding prepareSky(ICommandBuffer &commandBuffer) = 0;
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
    /** Drop the temporal resolve's history: a camera cut it cannot reproject. */
    void restartTemporalHistory();
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
    uint32_t _shadowCasterCategories {kAllShadowCasters};

    std::unique_ptr<GBuffer> _gbuffer;
    std::unique_ptr<IImage> _output;
    /** The other half of the tail's ping-pong; identical to _output, and
        swapped with it after every tail pass so the finished image is always
        _output. Allocated once, never per frame. */
    std::unique_ptr<IImage> _tailColor;
    /** Present only while the anti-aliasing slot is running a temporal
        resolve; it owns device memory of its own, so the choice is fixed for
        the lifetime of these targets rather than per frame. */
    std::unique_ptr<IUpscaler> _upscaler;
    glm::vec3 _prevCameraPosition {0.0f};
    bool _temporalHistoryValid {false};
    std::unique_ptr<IImage> _dirShadows;
    std::unique_ptr<IImage> _pointShadows;
    std::shared_ptr<Texture> _outputHandle;
    DescriptorSet _retroResolveSet;
    DescriptorSet _pbrResolveSet;
    DescriptorSet _resolveMaterialSet;
    GpuScene::View _mergedScene;
    bool _mergedScenePrepared {false};
    /**
     * This frame's sky, prepared before the resolve reads it.
     *
     * The bake records six cube-face passes on the frame a room changes, so it
     * cannot happen inside a pass; the resolve is inside no pass either, but it
     * is a consumer, so the bake runs once at the top of the frame and both
     * resolves bind what it left.
     */
    SkyBinding _skyBinding;
    /**
     * The occlusion kernel, in tangent space, generated once.
     *
     * Deterministic rather than drawn from the render random stream: a kernel
     * that differed between runs - or between one graphics rebuild and the
     * next - would make two captures of the same frame differ for a reason that
     * is not the renderer.
     */
    std::array<glm::vec4, kNumSSAOSamples> _ssaoKernel {};

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
    /** A second dispatch over the resolved image; see SceneStep. */
    void screenSpaceReflectionPass(ICommandBuffer &cmd, uint32_t globalsOffset);
    /** The two bindings a resolve cannot hold in its persistent table. */
    DescriptorSet resolveSet(IImage *output);
    /** This frame's rasterized primary, as the tracer is handed it. */
    GBufferBinding gbufferBinding();
    /** Word zero of the resolve push constants, from this frame's state. */
    uint32_t resolveFlags() const;
    /** The common anti-aliasing slot; the option selects the occupant. */
    void antiAliasingPass(ICommandBuffer &cmd, uint32_t globalsOffset);
    /** The temporal occupant of that slot, reprojecting through the G-buffer. */
    void upscalePass(ICommandBuffer &cmd);
    /** The one place a mode's colour becomes display-referred. */
    void postProcessPass(ICommandBuffer &cmd, uint32_t globalsOffset);
    void debugViewPass(ICommandBuffer &cmd, uint32_t globalsOffset);
    void sharpenPass(ICommandBuffer &cmd, uint32_t globalsOffset);
    /** One tail pass: full-screen triangle from _output onto _tailColor, then
        the swap that makes the result the output. */
    void tailPass(ICommandBuffer &cmd, const char *fragmentEntry,
                  uint32_t globalsOffset, uint32_t screenEffectOffset,
                  const void *pushConstants, uint32_t pushConstantSize);
    std::vector<Target> targetEntries(const ISceneCallbacks &callbacks) const;
};

} // namespace reone::graphics
