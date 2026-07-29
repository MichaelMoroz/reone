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

#include <NRD.h>

namespace reone {

namespace graphics {

class VulkanDevice;

}

namespace scene {

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
class NrdDenoiser : boost::noncopyable {
public:
    /** The trace pass's split, one view per NRD input. */
    struct Inputs {
        VkImageView motion {VK_NULL_HANDLE};
        VkImageView normalRoughness {VK_NULL_HANDLE};
        VkImageView viewZ {VK_NULL_HANDLE};
        VkImageView diffRadianceHitDist {VK_NULL_HANDLE};
        VkImageView specRadianceHitDist {VK_NULL_HANDLE};
    };

    /**
     * Live REBLUR tuning, mirrored from the Path tracing panel's dials.
     * Defaults match GraphicsOptions - the user-graded 2026-07-29 values.
     */
    struct Tuning {
        int maxAccumulatedFrames {6};
        int maxFastAccumulatedFrames {1};
        int maxStabilizedFrames {30};
        int historyFixFrames {4};
        float diffusePrepassBlurRadius {1.0f};
        float specularPrepassBlurRadius {1.0f};
        float minBlurRadius {0.5f};
        float maxBlurRadius {32.0f};
        float lobeAngleFraction {0.77f};
        float roughnessFraction {0.74f};
        float planeDistanceSensitivity {0.099f};
        float disocclusionThreshold {0.003f};
        bool antiFirefly {true};
    };

    NrdDenoiser(graphics::VulkanDevice &device, nrd::Instance &instance, glm::ivec2 extent) :
        _device(device), _instance(instance), _extent(extent) {}

    ~NrdDenoiser() { deinit(); }

    void init();
    void deinit();

    /**
     * Record this frame's denoising dispatches. The caller has already
     * barriered the trace pass's writes into compute-read visibility; the
     * denoised results land in denoisedDiffuse()/denoisedSpecular(), left in
     * GENERAL for the composite.
     */
    void denoise(VkCommandBuffer cmd,
                 int frameIndex,
                 const Inputs &inputs,
                 const Tuning &tuning,
                 const glm::mat4 &view,
                 const glm::mat4 &projection,
                 const glm::vec2 &jitter,
                 uint32_t frameNumber,
                 bool restartHistory);

    graphics::VulkanImage &denoisedDiffuse() { return *_outDiffuse; }
    graphics::VulkanImage &denoisedSpecular() { return *_outSpecular; }

private:
    graphics::VulkanDevice &_device;
    nrd::Instance &_instance;
    glm::ivec2 _extent;

    struct Pipeline {
        VkPipeline handle {VK_NULL_HANDLE};
        VkPipelineLayout layout {VK_NULL_HANDLE};
        /** One set layout per register space NRD declares (0 = cb+samplers space, possibly shared). */
        std::vector<std::pair<uint32_t, VkDescriptorSetLayout>> setLayouts;
    };

    std::vector<Pipeline> _pipelines;
    std::vector<std::unique_ptr<graphics::VulkanImage>> _permanentPool;
    std::vector<std::unique_ptr<graphics::VulkanImage>> _transientPool;
    std::unique_ptr<graphics::VulkanImage> _outDiffuse;
    std::unique_ptr<graphics::VulkanImage> _outSpecular;
    std::array<VkSampler, 2> _samplers {};
    std::array<VkDescriptorPool, 2> _descriptorPools {};
    std::unique_ptr<graphics::VulkanBuffer> _constants;
    VkDeviceSize _constantSlotSize {0};
    uint32_t _constantSlotsPerFrame {0};
    glm::mat4 _prevView {1.0f};
    glm::mat4 _prevProjection {1.0f};
    glm::vec2 _prevJitter {0.0f};
    glm::vec3 _prevCameraPosition {0.0f};
    bool _hasHistory {false};
    bool _poolTransitioned {false};

    VkImageView viewFor(const nrd::ResourceDesc &resource, const Inputs &inputs) const;
};

} // namespace scene

} // namespace reone

#endif
