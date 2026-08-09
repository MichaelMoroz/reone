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

#include <functional>
#include <memory>

#include "rhi.h"
#include "tracingpipeline.h"
#include "upscaler.h"
#include "../rendering/gpuscenecontext.h"

struct ImDrawData;
struct SDL_Window;

namespace reone {

namespace graphics {

class Texture;
class ICommandBuffer;
class IComputePipeline;
class IDescriptors;
class IImage;
class I2DRenderer;
class IPBRTextures;
class IPipelineCache;
class IResources;
class IUniformRing;

/**
 * Owns the frame: the target everything is drawn into, and how a finished frame
 * reaches the screen.
 *
 * This exists so that callers do not have to know what a frame is made of. In
 * OpenGL it is the default framebuffer and a window buffer swap; in Vulkan it
 * will be an acquired swapchain image, a recorded command buffer and a queue
 * present. Those differ enough that every call site which currently binds a
 * shader and draws a fullscreen quad would otherwise need a second version.
 *
 * A frame is strictly beginFrame, drawing, endFrame. Nothing may be drawn
 * outside that span.
 */
class IRenderer : public IGpuSceneContext {
public:
    virtual ~IRenderer() = default;

    virtual void init() = 0;
    virtual void deinit() = 0;

    /** Bind ImGui's platform and renderer backends to this renderer. */
    virtual void initImGui() = 0;

    /** Begin ImGui's renderer backend frame after the application has a context. */
    virtual void beginImGuiFrame() = 0;

    /** Draw ImGui into the frame currently owned by this renderer. */
    virtual void renderImGui(ImDrawData &drawData) = 0;

    /** Release ImGui backend resources before this renderer is torn down. */
    virtual void deinitImGui() = 0;

    /**
     * Acquire and clear a frame covering the whole of @p extent, in pixels.
     * Everything drawn until endFrame targets it.
     */
    virtual void beginFrame(glm::ivec2 extent) = 0;

    /**
     * Composite an offscreen scene render over the frame, filling it. This is
     * the hand-off from a render pipeline, which produces a texture, to the
     * backend, which decides how that texture becomes visible.
     */
    virtual void drawSceneOutput(Texture &output) = 0;

    /** Composite an offscreen scene render as the only 2D draw in the frame. */
    virtual void presentSceneOutput(Texture &output) {
        drawSceneOutput(output);
    }

    /**
     * Read the frame back as drawn so far, for screenshots and comparisons.
     *
     * Must be called before endFrame: once a frame is presented, the contents
     * of its target are undefined. Expect this to stall the pipeline.
     */
    virtual std::shared_ptr<Texture> captureFrame() = 0;

    /**
     * Finish the frame and present it, if this renderer owns presentation. A
     * renderer drawing into a target owned by someone else - the toolkit's
     * canvas, say - ends the frame without presenting.
     */
    virtual void endFrame() = 0;

    /** Device-side resources used to assign material texture ids. */
    virtual IResources &resources() = 0;

    /** Descriptor allocation and update for the frame being recorded. */
    virtual IDescriptors &descriptors() = 0;

    /** Per-frame uniform storage addressed by dynamic offsets. */
    virtual IUniformRing &uniformRing() = 0;

    /** Cached pipelines selected by backend-free pipeline keys. */
    virtual IPipelineCache &pipelines() = 0;

    /** Derived environment-map management for PBR material admission. */
    virtual IPBRTextures &pbrTextures() = 0;

    /** The screen-space batcher used while the renderer owns the 2D scope. */
    virtual I2DRenderer &renderer2d() = 0;

    /** Compile a compute pipeline whose descriptor layout comes from Slang. */
    virtual std::unique_ptr<IComputePipeline> makeComputePipeline(
        const ComputePipelineDesc &desc) = 0;

    /** Reflect the named runtime Slang module through the renderer boundary. */
    virtual ShaderReflection reflection(const std::string &name) const = 0;

    /** Compile and reflect the tracer's ray-generation pipeline. */
    virtual std::unique_ptr<ITracingPipeline> makeTracingPipeline(
        const TracingPipelineDesc &desc) = 0;

    /** Create the vendor denoiser behind its image-based tracing interface. */
    virtual std::unique_ptr<ITracingDenoiser> makeTracingDenoiser(glm::ivec2 extent) = 0;

    /**
     * Create the vendor temporal upscaler behind its image-based interface.
     *
     * @param highDynamicRange the colour it will be handed is linear and may
     *                         exceed one, as the traced chain's is; false for a
     *                         display-referred image, as every raster resolve
     *                         writes. The two need different internal handling
     *                         and the choice is fixed for the object's life.
     */
    /**
     * Create the vendor temporal upscaler. Two extents because it is the one
     * stage that changes resolution: it reads renderExtent and writes
     * displayExtent. Equal extents is NativeAA, a temporal resolve with no
     * upscaling.
     */
    virtual std::unique_ptr<IUpscaler> makeUpscaler(glm::ivec2 renderExtent,
                                                    glm::ivec2 displayExtent,
                                                    bool highDynamicRange) = 0;

    /** Create the frame-local structure used by the trace pass. */
    virtual std::unique_ptr<ITracingStructure> makeTracingStructure() = 0;

    /** The frame slot currently being recorded. */
    virtual int frameIndex() const = 0;

    /** The command buffer recording the current frame. */
    virtual ICommandBuffer &recordingCommandBuffer() = 0;

    /** The pixel format a scene output must use before presentation. */
    virtual Format sceneOutputFormat() const = 0;

    /** Rebuild shader modules, retaining prior modules if source compilation fails. */
    virtual bool recompileShaders() = 0;

    /** Submit completed recording so synchronous readback can observe this frame. */
    virtual void flushFrame() = 0;

    /** Recreate presentation resources with the requested synchronization mode. */
    virtual void setVsync(bool enabled) = 0;

    /** Record a scoped run of screen-space drawing in the current frame. */
    virtual void with2DRendering(glm::ivec2 logicalExtent,
                                 const std::function<void()> &block) = 0;

    /** Wait for all device work before releasing resources it may still use. */
    virtual void waitIdle() = 0;

    /** Whether this renderer can execute the path-tracing scene mode. */
    virtual bool rayQueryAvailable() const = 0;

    /** Record and complete short setup work outside a frame. */
    virtual void immediateSubmit(const std::function<void(ICommandBuffer &)> &block) = 0;

    /** Register an image for direct ImGui display and return its texture handle. */
    virtual void *addPreviewTexture(const IImage &image) = 0;

    /** Release an ImGui image handle returned by addPreviewTexture. */
    virtual void removePreviewTexture(void *texture) = 0;

    /**
     * Drop every cached device-side copy of an engine resource.
     *
     * Must be called when the engine frees Meshes and Textures in bulk - a
     * module transition. A backend that caches them by address cannot otherwise
     * tell that an address has been reused by a different object, and would
     * hand a new mesh the previous one's buffers.
     */
    virtual void invalidateResources() {}

    /**
     * Drop the device-side copy of one texture whose pixels have changed.
     *
     * Asset textures are immutable after upload, but video reuses one Texture
     * object for every decoded frame. Backends which cache by object identity
     * need this narrower lifetime boundary without discarding unrelated assets.
     */
    virtual void invalidateTexture(Texture &) {}
};

/** Create the renderer selected by this Vulkan-only build. */
std::unique_ptr<IRenderer> makeRenderer(SDL_Window *window, glm::ivec2 extent,
                                        bool vsync, bool validation);

} // namespace graphics

} // namespace reone
