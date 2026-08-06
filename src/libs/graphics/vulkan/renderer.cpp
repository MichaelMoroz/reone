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

#include "reone/graphics/vulkan/renderer.h"

#ifdef R_ENABLE_FSR
#include "reone/graphics/vulkan/fsrupscaler.h"
#endif
#ifdef R_ENABLE_NRD
#include "reone/graphics/vulkan/nrddenoiser.h"
#endif
#include "reone/graphics/vulkan/tracingstructure.h"

#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/pipeline.h"

#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include "reone/graphics/vulkan/renderpass.h"

#include "SDL3/SDL.h"

#include "reone/system/profiler.h"

#include "reone/graphics/texture.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/pipeline.h"
#include "reone/system/logutil.h"

namespace reone {

namespace graphics {

std::unique_ptr<IRenderer> makeRenderer(SDL_Window *window, glm::ivec2 extent,
                                        bool vsync, bool validation) {
    return std::make_unique<VulkanRenderer>(window, extent, vsync, validation);
}

static void check(VkResult result, const char *what) {
    if (result == VK_SUCCESS) {
        return;
    }
    // Device loss is worth naming rather than leaving as a number. It is what a
    // display driver watchdog reset looks like from here, and the validation
    // layers say nothing about it because nothing was used incorrectly - a
    // shader simply ran for longer than the driver was willing to wait. Whatever
    // is thrown next tends to surface far from the pass that caused it, so the
    // log line is the only thing tying the two together.
    if (result == VK_ERROR_DEVICE_LOST) {
        error(str(boost::format("Vulkan: device lost during %s - the GPU was reset, "
                                "most likely by the driver watchdog timing out a "
                                "long-running shader") %
                  what),
              LogChannel::Graphics);
    }
    throw std::runtime_error(str(boost::format("Vulkan: %s failed (%d)") % what % result));
}

void VulkanRenderer::init() {
    if (_inited) {
        return;
    }
    _device.init(_window, _validation);
    _swapchain.init(_extent, _vsync);
    _requestedExtent = _extent;
    initFrames();
    initImageSemaphores();
    // 16 MB per frame. The original 1 MB was a guess and a Dantooine exterior
    // overran it: dangly meshes push a 12 KB DanglyUniforms block per draw, and
    // a stand of trees is a lot of those. Measured peak there is just under
    // 7 MB, so this leaves better than twice the headroom. peakUsage() is
    // logged on shutdown, and exhausting the arena throws with the number
    // rather than corrupting anything.
    _depth = std::make_unique<VulkanImage>(_device);
    _depth->initDepth(_swapchain.extent(), kDepthFormat);
    _uniformRing.init(kFramesInFlight, 16u << 20);
    _descriptors.init(kFramesInFlight, _uniformRing);
    _pbrTextures.init();
    // The source tree wins wherever it exists, so that editing a shader and
    // asking for a recompile compiles the file that was edited. The build
    // deposits a copy beside the executable, and preferring that copy is what
    // made runtime recompilation appear not to work at all: the reload
    // faithfully rebuilt a stale duplicate of every module, including the
    // imported ones, and the frame never changed. An installed build has no
    // source tree and takes the copy, which is what it is for.
    bool sourceDirSet = false;
#ifdef REONE_SHADER_SOURCE_DIR
    std::filesystem::path shaderSource {REONE_SHADER_SOURCE_DIR};
    if (std::filesystem::is_directory(shaderSource)) {
        _shaderCompiler.setSourceDir(std::move(shaderSource));
        sourceDirSet = true;
    }
#endif
    if (!sourceDirSet) {
        if (auto *base = SDL_GetBasePath()) {
            auto deployedSource = std::filesystem::path(base) / "slang";
            SDL_free(const_cast<char *>(base));
            if (std::filesystem::is_directory(deployedSource))
                _shaderCompiler.setSourceDir(std::move(deployedSource));
        }
    }
    _shaderCompiler.init();
    _shaderCompiler.validateSchemas();
    initPipelineCache();
    _renderer2d.init();
    _inited = true;
}

void VulkanRenderer::initPipelineCache() {
    _pipelines.init(
        [this](const std::string &name) {
            return shaderModule(name);
        });
}

bool VulkanRenderer::recompileShaders() {
    if (_inFrame)
        throw std::runtime_error("Vulkan: shader reload requested while a frame is recording");
    vkDeviceWaitIdle(_device.handle());
    const bool success = _shaderCompiler.recompileAll();
    // Pipeline creation retains shader modules internally. Rebuild the cache so
    // subsequent draws use the refreshed SPIR-V (or the retained last-good one).
    _pipelines.deinit();
    initPipelineCache();
    return success;
}

void VulkanRenderer::deinit() {
    if (!_inited) {
        return;
    }
    // Nothing may be destroyed while the GPU might still be reading it.
    vkDeviceWaitIdle(_device.handle());
    info(str(boost::format("Vulkan: peak uniform arena usage %llu bytes") %
             _uniformRing.peakUsage()));
    _pbrTextures.deinit();
    _renderer2d.deinit();
    _resources.deinit();
    _pipelines.deinit();
    _shaderCompiler.deinit();
    _descriptors.deinit();
    _uniformRing.deinit();
    _depth.reset();
    deinitImageSemaphores();
    deinitFrames();
    _swapchain.deinit();
    _device.deinit();
    _inited = false;
}

void VulkanRenderer::initImGui() {
    if (!ImGui_ImplSDL3_InitForVulkan(_window)) {
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui: SDL Vulkan backend initialization failed");
    }

    VkFormat colorFormat = _swapchain.imageFormat();

    ImGui_ImplVulkan_InitInfo info {};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = _device.instance();
    info.PhysicalDevice = _device.physicalDevice();
    info.Device = _device.handle();
    info.QueueFamily = _device.graphicsQueueFamily();
    info.Queue = _device.graphicsQueue();
    info.DescriptorPoolSize = 64;
    info.MinImageCount = 2;
    info.ImageCount = _swapchain.imageCount() < 2u ? 2u : _swapchain.imageCount();
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;

    if (!ImGui_ImplVulkan_Init(&info)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        throw std::runtime_error("ImGui: Vulkan renderer backend initialization failed");
    }
}

void VulkanRenderer::beginImGuiFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
}

void VulkanRenderer::renderImGui(ImDrawData &drawData) {
    if (!_inFrame) {
        return;
    }

    RenderPassScope rendering(
        commandBuffer(), _swapchain.extent(),
        {{currentImageView(),
          VK_IMAGE_LAYOUT_GENERAL,
          VK_ATTACHMENT_LOAD_OP_LOAD,
          VK_ATTACHMENT_STORE_OP_STORE}});
    ImGui_ImplVulkan_RenderDrawData(&drawData, commandBuffer());
}

void VulkanRenderer::deinitImGui() {
    // The last submitted frame may still reference the font texture,
    // descriptor sets, and pipeline owned by the backend.
    _device.waitIdle();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void VulkanRenderer::initFrames() {
    VkCommandPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = _device.graphicsQueueFamily();

    VkSemaphoreCreateInfo semInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    // Created signalled, so the first frame does not wait for a submission that
    // never happened.
    VkFenceCreateInfo fenceInfo {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (auto &frame : _frames) {
        check(vkCreateCommandPool(_device.handle(), &poolInfo, nullptr, &frame.commandPool),
              "vkCreateCommandPool");

        VkCommandBufferAllocateInfo allocInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = frame.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(_device.handle(), &allocInfo, &frame.commandBuffer),
              "vkAllocateCommandBuffers");

        check(vkCreateSemaphore(_device.handle(), &semInfo, nullptr, &frame.imageAvailable),
              "vkCreateSemaphore");
        check(vkCreateFence(_device.handle(), &fenceInfo, nullptr, &frame.inFlight),
              "vkCreateFence");
    }
}

void VulkanRenderer::deinitFrames() {
    for (auto &frame : _frames) {
        if (frame.inFlight) {
            vkDestroyFence(_device.handle(), frame.inFlight, nullptr);
        }
        if (frame.imageAvailable) {
            vkDestroySemaphore(_device.handle(), frame.imageAvailable, nullptr);
        }
        if (frame.commandPool) {
            vkDestroyCommandPool(_device.handle(), frame.commandPool, nullptr);
        }
        frame = Frame {};
    }
}

void VulkanRenderer::initImageSemaphores() {
    VkSemaphoreCreateInfo semInfo {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    _renderFinished.resize(_swapchain.imageCount());
    for (auto &sem : _renderFinished) {
        check(vkCreateSemaphore(_device.handle(), &semInfo, nullptr, &sem), "vkCreateSemaphore");
    }
}

void VulkanRenderer::deinitImageSemaphores() {
    for (auto sem : _renderFinished) {
        vkDestroySemaphore(_device.handle(), sem, nullptr);
    }
    _renderFinished.clear();
}

void VulkanRenderer::transitionImage(VkCommandBuffer cmd,
                                     VkImage image,
                                     VkImageLayout from,
                                     VkImageLayout to) {
    VkImageMemoryBarrier2 barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    // Deliberately conservative: correctness first, and a barrier per frame is
    // not where any time goes. Tighten the stage and access masks once there
    // are real passes with real dependencies between them.
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dep {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void VulkanRenderer::beginFrame(glm::ivec2 extent) {
    if (_inFrame) {
        throw std::logic_error("Renderer: frame already begun");
    }
    if (_needsRecreate || extent != _requestedExtent) {
        _swapchain.recreate(extent);
        // The image count can change with the swapchain, and any pending
        // present on the old semaphores is finished by the wait inside
        // recreate, so they are safe to replace here.
        deinitImageSemaphores();
        initImageSemaphores();
        _depth = std::make_unique<VulkanImage>(_device);
        _depth->initDepth(_swapchain.extent(), kDepthFormat);
        _needsRecreate = false;
        _requestedExtent = extent;
    }
    _extent = extent;

    auto &frame = _frames[_frameIndex];
    check(vkWaitForFences(_device.handle(), 1, &frame.inFlight, VK_TRUE, UINT64_MAX),
          "vkWaitForFences");

    auto acquired = vkAcquireNextImageKHR(
        _device.handle(), _swapchain.handle(), UINT64_MAX,
        frame.imageAvailable, VK_NULL_HANDLE, &_imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        // The image was never acquired, so the semaphore was not signalled and
        // there is nothing to submit. Rebuild and take the image on the retry.
        _swapchain.recreate(extent);
        deinitImageSemaphores();
        initImageSemaphores();
        acquired = vkAcquireNextImageKHR(
            _device.handle(), _swapchain.handle(), UINT64_MAX,
            frame.imageAvailable, VK_NULL_HANDLE, &_imageIndex);
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        check(acquired, "vkAcquireNextImageKHR");
    }

    // Reset only once we know we will submit; a fence reset without a matching
    // submit deadlocks the next frame's wait.
    check(vkResetFences(_device.handle(), 1, &frame.inFlight), "vkResetFences");
    check(vkResetCommandBuffer(frame.commandBuffer, 0), "vkResetCommandBuffer");

    // Safe now, and only now: the fence above says the GPU has finished every
    // draw that was reading this arena.
    _uniformRing.beginFrame(_frameIndex);
    _descriptors.beginFrame(_frameIndex);

    VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "vkBeginCommandBuffer");
    frame.recordingCommandBuffer.begin(frame.commandBuffer, &_device, _frameIndex);

    auto image = _swapchain.image(_imageIndex);
    transitionImage(frame.commandBuffer, image,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    // Clear here rather than as a render pass load op: there are no attachments
    // yet, and this keeps beginFrame meaning the same thing it does in GL.
    VkClearColorValue clear {{_clearColor.r, _clearColor.g, _clearColor.b, _clearColor.a}};
    VkImageSubresourceRange range {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = VK_REMAINING_MIP_LEVELS;
    range.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdClearColorImage(frame.commandBuffer, image,
                         VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);

    _inFrame = true;
}

void VulkanRenderer::drawSceneOutput(Texture &output) {
    if (!_inFrame) {
        throw std::logic_error("Renderer: no frame begun");
    }
    if (!_in2DRendering) {
        throw std::logic_error("Renderer: no 2D rendering scope begun");
    }
    // The scene pipeline registered its output image against this Texture, so
    // the 2D path composites it like any other full-target image, orientation
    // included.
    _renderer2d.drawFullTargetImage(output);
}

void VulkanRenderer::with2DRendering(glm::ivec2 logicalExtent,
                                      const std::function<void()> &block) {
    if (!_inFrame) {
        throw std::logic_error("Renderer: no frame begun");
    }
    if (_in2DRendering) {
        throw std::logic_error("Renderer: 2D rendering scope already begun");
    }

    auto cmd = commandBuffer();
    const auto physicalExtent = _swapchain.extent();
    // The swapchain can be smaller than the requested client extent. Dynamic
    // rendering targets physical pixels, while Renderer2D keeps its
    // projection in the logical extent so the result scales rather than crops.
    VulkanDebugScope debugScope(
        _device, cmd, "2D (scene composite, GUI, console)",
        glm::vec3 {0.9f, 0.9f, 0.4f});
    RenderPassScope rendering(
        cmd, physicalExtent,
        {{currentImageView(),
          VK_IMAGE_LAYOUT_GENERAL,
          VK_ATTACHMENT_LOAD_OP_LOAD,
          VK_ATTACHMENT_STORE_OP_STORE}});
    _renderer2d.begin(recordingCommandBuffer(), logicalExtent, physicalExtent,
                      fromVulkanFormat(_swapchain.imageFormat()));
    _in2DRendering = true;
    try {
        block();
    } catch (...) {
        _renderer2d.end();
        _in2DRendering = false;
        throw;
    }
    _renderer2d.end();
    _in2DRendering = false;
}

void VulkanRenderer::presentSceneOutput(Texture &output) {
    with2DRendering(_extent, [this, &output]() { drawSceneOutput(output); });
}

std::shared_ptr<Texture> VulkanRenderer::captureFrame() {
    if (!_inFrame) {
        throw std::logic_error("Renderer: no frame begun");
    }
    auto &frame = _frames[_frameIndex];
    auto image = _swapchain.image(_imageIndex);
    auto extent = _swapchain.extent();
    VkDeviceSize size = static_cast<VkDeviceSize>(extent.x) * extent.y * 4;

    VkBufferCreateInfo bufInfo {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = size;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocInfo {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer staging {VK_NULL_HANDLE};
    VmaAllocation allocation {VK_NULL_HANDLE};
    VmaAllocationInfo allocated {};
    check(vmaCreateBuffer(_device.allocator(), &bufInfo, &allocInfo,
                          &staging, &allocation, &allocated),
          "vmaCreateBuffer");

    transitionImage(frame.commandBuffer, image,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {static_cast<uint32_t>(extent.x), static_cast<uint32_t>(extent.y), 1};
    vkCmdCopyImageToBuffer(frame.commandBuffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);

    transitionImage(frame.commandBuffer, image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

    flushFrame();

    // The swapchain format is B8G8R8A8; Texture wants RGB8. Rows are also
    // reversed: a Vulkan image copy yields them top-down, while glReadPixels
    // yields bottom-up, and everything downstream - TgaWriter, the comparison
    // harness - assumes the OpenGL order. Without this the screenshot and the
    // window disagree, which is worse than either being wrong.
    auto pixels = std::make_shared<ByteBuffer>();
    pixels->resize(static_cast<size_t>(extent.x) * extent.y * 3);
    auto src = static_cast<const uint8_t *>(allocated.pMappedData);
    for (int y = 0; y < extent.y; ++y) {
        auto srcRow = src + static_cast<size_t>(y) * extent.x * 4;
        auto dstRow = &(*pixels)[static_cast<size_t>(extent.y - 1 - y) * extent.x * 3];
        for (int x = 0; x < extent.x; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 2];
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 4 + 0];
        }
    }
    vmaDestroyBuffer(_device.allocator(), staging, allocation);

    auto texture = std::make_shared<Texture>("screenshot", TextureType::TwoDim, Texture::Properties());
    texture->setPixels(extent.x, extent.y, PixelFormat::RGB8, Texture::Layer {pixels});

    return texture;
}

void VulkanRenderer::flushFrame() {
    if (!_inFrame) {
        throw std::logic_error("Renderer: no frame begun");
    }
    if (_imageAvailableConsumed) {
        // captureFrame already split this frame; its follow-up buffer only
        // contains endFrame work, so another flush would add no target data.
        return;
    }
    auto &frame = _frames[_frameIndex];

    // Readback needs the current frame's commands to finish, but endFrame still
    // owns the transition and present, so it continues in a fresh buffer.
    frame.recordingCommandBuffer.end();
    check(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cmdInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = frame.commandBuffer;

    VkSemaphoreSubmitInfo waitInfo {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = frame.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo2 submit {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    check(vkQueueSubmit2(_device.graphicsQueue(), 1, &submit, VK_NULL_HANDLE),
          "vkQueueSubmit2");
    check(vkQueueWaitIdle(_device.graphicsQueue()), "vkQueueWaitIdle");

    // imageAvailable was consumed by the submit above, so endFrame must not
    // wait on it again.
    check(vkResetCommandBuffer(frame.commandBuffer, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo beginInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo), "vkBeginCommandBuffer");
    frame.recordingCommandBuffer.begin(frame.commandBuffer, &_device, _frameIndex);
    _imageAvailableConsumed = true;
}

void VulkanRenderer::invalidateResources() {
    // The GPU may still be reading anything uploaded, and this is a load-time
    // operation, so the blunt wait is the right one.
    _device.waitIdle();
    _resources.clearUploaded();
    _pbrTextures.refresh();
}

void VulkanRenderer::invalidateTexture(Texture &texture) {
    _resources.invalidate(texture);
}

void VulkanRenderer::endFrame() {
    R_PROFILE_ZONE("VulkanRenderer::present");
    if (!_inFrame) {
        throw std::logic_error("Renderer: no frame begun");
    }
    if (_in2DRendering) {
        throw std::logic_error("Renderer: 2D rendering scope not ended");
    }
    auto &frame = _frames[_frameIndex];

    transitionImage(frame.commandBuffer, _swapchain.image(_imageIndex),
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    frame.recordingCommandBuffer.end();
    check(vkEndCommandBuffer(frame.commandBuffer), "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cmdInfo {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = frame.commandBuffer;

    VkSemaphoreSubmitInfo waitInfo {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = frame.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfo {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = _renderFinished[_imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    VkSubmitInfo2 submit {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    // A capture already waited on and consumed imageAvailable; waiting again
    // would block on a semaphore nothing will signal.
    submit.waitSemaphoreInfoCount = _imageAvailableConsumed ? 0 : 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    check(vkQueueSubmit2(_device.graphicsQueue(), 1, &submit, frame.inFlight),
          "vkQueueSubmit2");
    _imageAvailableConsumed = false;

    auto swapchain = _swapchain.handle();
    VkPresentInfoKHR present {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &_renderFinished[_imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &_imageIndex;

    auto presented = vkQueuePresentKHR(_device.graphicsQueue(), &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        _needsRecreate = true;
    } else {
        check(presented, "vkQueuePresentKHR");
    }

    _frameIndex = (_frameIndex + 1) % kFramesInFlight;
    _inFrame = false;
}

std::unique_ptr<IBuffer> VulkanRenderer::makeBuffer() {
    return std::make_unique<VulkanBuffer>(_device);
}

void VulkanRenderer::immediateSubmit(const std::function<void(ICommandBuffer &)> &block) {
    _device.immediateSubmit([this, &block](VkCommandBuffer native) {
        VulkanCommandBuffer commandBuffer;
        commandBuffer.begin(native, &_device, 0);
        block(commandBuffer);
        commandBuffer.end();
    });
}

void *VulkanRenderer::addPreviewTexture(const IImage &image) {
    const auto &native = toVulkanImage(image);
    return ImGui_ImplVulkan_AddTexture(native.sampler(), native.view(),
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanRenderer::removePreviewTexture(void *texture) {
    ImGui_ImplVulkan_RemoveTexture(static_cast<VkDescriptorSet>(texture));
}

std::unique_ptr<IComputePipeline> VulkanRenderer::makeComputePipeline(
    const ComputePipelineDesc &desc) {
    auto pipeline = std::make_unique<VulkanComputePipeline>(
        _device, _descriptors, _uniformRing, desc, shaderModule(desc.shader),
        _shaderCompiler.reflection(desc.shader));
    pipeline->init();
    _device.setObjectName(VK_OBJECT_TYPE_PIPELINE,
                          reinterpret_cast<uint64_t>(toVulkanComputePipeline(*pipeline).handle()),
                          "compute:" + desc.shader);
    return pipeline;
}

std::unique_ptr<ITracingPipeline> VulkanRenderer::makeTracingPipeline(
    const TracingPipelineDesc &desc) {
    return _pipelines.makeTracingPipeline(shaderModule(desc.shader),
                                          desc.reflection,
                                          _device.maxBindlessSampledImages(),
                                          desc.pushConstantSize, desc.label);
}

std::unique_ptr<ITracingDenoiser> VulkanRenderer::makeTracingDenoiser(glm::ivec2 extent) {
#ifdef R_ENABLE_NRD
    return ::reone::graphics::makeTracingDenoiser(_device, extent);
#else
    return nullptr;
#endif
}

std::unique_ptr<IUpscaler> VulkanRenderer::makeUpscaler(glm::ivec2 extent,
                                                        bool highDynamicRange) {
#ifdef R_ENABLE_FSR
    auto upscaler = std::make_unique<FsrUpscaler>(_device, extent, highDynamicRange);
    upscaler->init();
    return upscaler;
#else
    return nullptr;
#endif
}

std::unique_ptr<ITracingStructure> VulkanRenderer::makeTracingStructure() {
    return graphics::makeTracingStructure(_device);
}

void VulkanRenderer::prepareMesh(const Mesh &mesh) {
    _resources.get(mesh);
}

uint64_t VulkanRenderer::resourceGeneration() const {
    return _resources.generation();
}

} // namespace graphics

} // namespace reone
