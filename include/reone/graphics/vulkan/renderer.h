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

#include <volk.h>

#include <functional>

#include "../renderer.h"
#include "../gpuscenecontext.h"

#include "descriptors.h"
#include "debugscope.h"
#include "device.h"
#include "swapchain.h"
#include "pipelinecache.h"
#include "renderer2d.h"
#include "commandbuffer.h"
#include "pbrtextures.h"
#include "resources.h"
#include "shadercompiler.h"
#include "uniformring.h"

struct SDL_Window;

namespace reone {

namespace graphics {

class Mesh;

/**
 * The frame as Vulkan sees it: acquire a swapchain image, record a command
 * buffer, submit, present.
 *
 * Frames overlap. kFramesInFlight sets of per-frame objects are cycled so the
 * CPU can record frame N+1 while the GPU is still working on frame N, and the
 * fence is what stops it from getting further ahead than that.
 */
class VulkanRenderer : public IRenderer, public IGpuSceneContext, boost::noncopyable {
public:
    static constexpr int kFramesInFlight = 2;

    /** Guaranteed present as a depth format on every implementation. */
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    VulkanRenderer(SDL_Window *window, glm::ivec2 extent, bool vsync, bool validation) :
        _window(window),
        _extent(extent),
        _vsync(vsync),
        _validation(validation),
        _swapchain(_device),
        _uniformRing(_device),
        _descriptors(_device),
        _shaderCompiler(REONE_SHADER_SOURCE_DIR),
        _pipelines(_device, _descriptors),
        _resources(_device),
        _pbrTextures(_device, _pipelines, _uniformRing, _descriptors, _resources),
        _renderer2d(_device, _pipelines, _uniformRing, _descriptors, _resources) {
    }

    ~VulkanRenderer() { deinit(); }

    void init() override;
    void deinit() override;

    void beginFrame(glm::ivec2 extent) override;
    void with2DRendering(glm::ivec2 logicalExtent, const std::function<void()> &block);
    void drawSceneOutput(Texture &output) override;
    void presentSceneOutput(Texture &output) override;
    std::shared_ptr<Texture> captureFrame() override;
    /** Submit completed recording so a synchronous readback can see this frame. */
    void flushFrame();
    void endFrame() override;
    void invalidateResources() override;
    void invalidateTexture(Texture &texture) override;
    void setVsync(bool enabled) {
        _swapchain.setVsync(enabled);
        _needsRecreate = true;
    }

    /** The colour beginFrame clears to. */
    void setClearColor(glm::vec4 color) { _clearColor = color; }

    std::unique_ptr<IBuffer> makeBuffer() override;
    std::unique_ptr<IGpuSceneMergePipeline> makeGpuSceneMergePipeline() override;
    void prepareMesh(const Mesh &mesh) override;
    uint64_t resourceGeneration() const override;

    VulkanDevice &device() { return _device; }
    VulkanUniformRing &uniformRing() { return _uniformRing; }
    VulkanDescriptors &descriptors() { return _descriptors; }
    VulkanPipelineCache &pipelines() { return _pipelines; }
    VulkanResources &resources() { return _resources; }
    VulkanPBRTextures &pbrTextures() { return _pbrTextures; }
    Vulkan2DRenderer &renderer2d() { return _renderer2d; }

    /** Runtime-compiled SPIR-V for one named Slang module. */
    const std::vector<uint32_t> &shaderModule(const std::string &name) {
        return _shaderCompiler.module(name);
    }
    /** Rebuild source modules now; bad sources retain their prior modules. */
    bool recompileShaders();

    /** The uniform descriptor set for the frame being recorded. */
    VkDescriptorSet uniformSet() const { return _descriptors.uniformSet(_frameIndex); }
    VulkanSwapchain &swapchain() { return _swapchain; }
    int frameIndex() const { return _frameIndex; }

    /** Whether a frame is open, and so whether recording is legal. */
    bool inFrame() const { return _inFrame; }

    /** The command buffer being recorded, valid only between begin and end. */
    VkCommandBuffer commandBuffer() const { return _frames[_frameIndex].commandBuffer; }
    ICommandBuffer &recordingCommandBuffer() {
        return _frames[_frameIndex].recordingCommandBuffer;
    }

    /** The image being rendered into this frame, and its view. */
    VkImageView currentImageView() const { return _swapchain.imageView(_imageIndex); }

    /** Depth attachment sized with the swapchain, recreated alongside it. */
    VkImageView depthView() const { return _depth->view(); }
    VkFormat depthFormat() const { return kDepthFormat; }

private:
    /**
     * Everything a frame in flight needs its own copy of. Sharing any of these
     * between overlapping frames is the classic source of validation errors
     * that only appear under load.
     */
    struct Frame {
        VkCommandPool commandPool {VK_NULL_HANDLE};
        VkCommandBuffer commandBuffer {VK_NULL_HANDLE};
        VulkanCommandBuffer recordingCommandBuffer;
        /** Signalled when the acquired image is ready to be rendered into. */
        VkSemaphore imageAvailable {VK_NULL_HANDLE};
        /** Signalled when the GPU is done, so the CPU may reuse this set. */
        VkFence inFlight {VK_NULL_HANDLE};
    };

    SDL_Window *_window;
    glm::ivec2 _extent;
    /** Last extent beginFrame was asked for. Compared instead of the
        swapchain's own extent, which the surface may have clamped smaller -
        comparing against the clamped value recreated the swapchain every
        frame whenever the OS shrank the window. */
    glm::ivec2 _requestedExtent {0};
    bool _vsync;
    bool _validation;
    glm::vec4 _clearColor {0.0f, 0.0f, 0.0f, 1.0f};

    VulkanDevice _device;
    VulkanSwapchain _swapchain;
    std::unique_ptr<VulkanImage> _depth;
    VulkanUniformRing _uniformRing;
    VulkanDescriptors _descriptors;
    SlangShaderCompiler _shaderCompiler;
    VulkanPipelineCache _pipelines;
    VulkanResources _resources;
    VulkanPBRTextures _pbrTextures;
    Vulkan2DRenderer _renderer2d;

    bool _inited {false};
    bool _inFrame {false};
    bool _in2DRendering {false};
    /**
     * Set when acquire or present reports the swapchain no longer matches the
     * window. Acted on at the start of the next frame rather than immediately,
     * because a half-recorded frame still has to be finished or abandoned
     * cleanly.
     */
    bool _needsRecreate {false};
    /** Set when a mid-frame flush consumes the acquire semaphore. */
    bool _imageAvailableConsumed {false};

    std::array<Frame, kFramesInFlight> _frames;
    int _frameIndex {0};
    uint32_t _imageIndex {0};

    /**
     * Signalled when rendering into a swapchain image is done and it may be
     * presented. One per swapchain image rather than per frame in flight:
     * presentation consumes this semaphore against a particular image, and
     * there is no signal that it has done so, so a per-frame semaphore can be
     * re-signalled while a present is still pending on it. The image count and
     * the in-flight count also need not match.
     */
    std::vector<VkSemaphore> _renderFinished;

    void initFrames();
    void deinitFrames();
    void initImageSemaphores();
    void deinitImageSemaphores();
    void initPipelineCache();

    /**
     * Swapchain images arrive in an undefined layout and must be presentable
     * when handed back, so every frame moves its image twice.
     */
    void transitionImage(VkCommandBuffer cmd,
                         VkImage image,
                         VkImageLayout from,
                         VkImageLayout to);
};

} // namespace graphics

} // namespace reone
