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

#ifdef R_ENABLE_NRD

#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/rhi/tracingpipeline.h"

#include <NRD.h>

#include <chrono>

namespace reone {

namespace graphics {

class VulkanDevice;

/**
 * Records NRD's denoising passes into the frame's command buffer, GAPI-free
 * on NRD's side: the library hands back compute dispatch descriptions -
 * pipeline index, embedded SPIRV, resource lists, constants - and this class
 * owns their Vulkan incarnation. Deliberately no NRI: its own Vulkan
 * entry-point resolution inside a volk-owned process is the same silent-crash
 * class as the VMA case.
 *
 * Exists only under ENABLE_NRD, which is a local developer toggle - the
 * NVIDIA RTX SDKs license and GPL-3 mutually exclude each other in a
 * distributed binary.
 */
class NrdDenoiser : public ITracingDenoiser, boost::noncopyable {
public:
    NrdDenoiser(VulkanDevice &device, nrd::Instance &instance, glm::ivec2 extent,
                bool ownsInstance = false,
                TracingDenoiserKind kind = TracingDenoiserKind::Relax) :
        _device(device), _instance(instance), _extent(extent), _ownsInstance(ownsInstance),
        _kind(kind) {}

    ~NrdDenoiser() override {
        deinit();
        if (_ownsInstance) nrd::DestroyInstance(_instance);
    }

    void init();
    void deinit();

    /**
     * Record this frame's denoising dispatches. The caller has already
     * barriered the trace pass's writes into compute-read visibility; the
     * denoised results land in denoisedDiffuse()/denoisedSpecular(), left in
     * GENERAL for the composite.
     */
    void denoise(ICommandBuffer &commandBuffer,
                 int frameIndex,
                 const TracingDenoiserInputs &inputs,
                 const TracingDenoiserTuning &tuning,
                 const glm::mat4 &view,
                 const glm::mat4 &projection,
                 const glm::vec2 &jitter,
                 uint32_t frameNumber,
                 bool restartHistory) override;

    IImage &denoisedDiffuse() override { return *_outDiffuse; }
    IImage &denoisedSpecular() override { return *_outSpecular; }

private:
    VulkanDevice &_device;
    nrd::Instance &_instance;
    bool _ownsInstance {false};
    glm::ivec2 _extent;

    struct Pipeline {
        VkPipeline handle {VK_NULL_HANDLE};
        VkPipelineLayout layout {VK_NULL_HANDLE};
        /** One set layout per register space NRD declares (0 = cb+samplers space, possibly shared). */
        std::vector<std::pair<uint32_t, VkDescriptorSetLayout>> setLayouts;
    };

    std::vector<Pipeline> _pipelines;
    std::vector<std::unique_ptr<VulkanImage>> _permanentPool;
    std::vector<std::unique_ptr<VulkanImage>> _transientPool;
    std::unique_ptr<VulkanImage> _outDiffuse;
    std::unique_ptr<VulkanImage> _outSpecular;
    std::array<VkSampler, 2> _samplers {};
    std::array<VkDescriptorPool, 2> _descriptorPools {};
    std::unique_ptr<VulkanBuffer> _constants;
    VkDeviceSize _constantSlotSize {0};
    uint32_t _constantSlotsPerFrame {0};
    glm::mat4 _prevView {1.0f};
    glm::mat4 _prevProjection {1.0f};
    glm::vec2 _prevJitter {0.0f};
    glm::vec3 _prevCameraPosition {0.0f};
    bool _hasHistory {false};
    TracingDenoiserKind _kind {TracingDenoiserKind::Relax};
    /**
     * Smoothed frame time, the denominator that turns an accumulation time into
     * the frame count NRD is configured with. Measured here rather than plumbed
     * through four layers, and smoothed because a single long frame - a shader
     * recompile, a level load - would otherwise collapse the history for the
     * frames that follow it.
     */
    float _frameTimeMs {16.667f};
    std::chrono::steady_clock::time_point _lastFrameTime {};

    VkImageView viewFor(const nrd::ResourceDesc &resource,
                        const TracingDenoiserInputs &inputs) const;
};

std::unique_ptr<ITracingDenoiser> makeTracingDenoiser(VulkanDevice &device,
                                                       glm::ivec2 extent,
                                                       TracingDenoiserKind kind);

} // namespace graphics

} // namespace reone

#endif
