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

#include <optional>
#include <string>
#include <vector>

#include "reone/graphics/rhi/computepipeline.h"
#include "reone/graphics/types.h"
#include "reone/graphics/vulkan/descriptorwrites.h"
#include "reone/graphics/vulkan/rhi.h"

namespace reone {

namespace graphics {

class VulkanDevice;
class VulkanDescriptors;
class VulkanUniformRing;

class VulkanPipeline : boost::noncopyable {
public:
    struct LayoutBinding {
        DescriptorBinding binding;
        uint32_t count {1};
        VkShaderStageFlags stages {0};
        VkDescriptorBindingFlags flags {0};
    };

    struct DescriptorSet {
        VkDescriptorSetLayout layout {VK_NULL_HANDLE};
        std::vector<LayoutBinding> bindings;
        uint32_t copies {0};
        VkDescriptorSetLayoutCreateFlags flags {0};
    };

    struct Config {
        enum class Type {
            Graphics,
            Compute,
            RayTracing,
        };

        Type type {Type::Graphics};
        std::vector<uint32_t> spirv;
        std::string vertexEntry;
        std::string fragmentEntry;
        std::string computeEntry;
        std::string raygenEntry;
        std::vector<VkFormat> colorFormats;
        VkFormat depthFormat {VK_FORMAT_UNDEFINED};
        uint32_t viewMask {0};
        BlendMode blend {BlendMode::None};
        FaceCullMode cull {FaceCullMode::None};
        bool depthTest {false};
        bool depthWrite {false};
        bool depthBias {false};
        float depthBiasConstantFactor {0.0f};
        float depthBiasSlopeFactor {0.0f};
        std::vector<DescriptorSet> descriptorSets;
        std::vector<VkPushConstantRange> pushConstants;
        /** A single range whose stage is implied by this pipeline's work. */
        uint32_t pushConstantSize {0};
        std::vector<VkVertexInputBindingDescription> vertexBindings;
        std::vector<VkVertexInputAttributeDescription> vertexAttributes;
    };

    VulkanPipeline(VulkanDevice &device) :
        _device(device) {
    }

    ~VulkanPipeline() { deinit(); }

    void init(const Config &config);
    void deinit();

    VkPipeline handle() const { return _pipeline; }
    VkPipelineLayout layout() const { return _layout; }
    VkDescriptorSet descriptorSet(uint32_t set, uint32_t copy) const;
    Pipeline pipeline() const { return toPipeline(_pipeline); }
    PipelineLayout pipelineLayout() const { return toPipelineLayout(_layout); }
    ::reone::graphics::DescriptorSet descriptorSetHandle(uint32_t set, uint32_t copy) const {
        return toDescriptorSet(descriptorSet(set, copy));
    }
private:
    VulkanDevice &_device;

    struct OwnedSet {
        VkDescriptorSetLayout layout {VK_NULL_HANDLE};
        VkDescriptorPool pool {VK_NULL_HANDLE};
    };

    VkPipeline _pipeline {VK_NULL_HANDLE};
    VkPipelineLayout _layout {VK_NULL_HANDLE};
    std::vector<VkDescriptorSetLayout> _setLayouts;
    std::vector<OwnedSet> _ownedSets;
    std::vector<std::vector<VkDescriptorSet>> _sets;
};

/** Vulkan implementation of a Slang-reflected compute pipeline. */
class VulkanComputePipeline : public IComputePipeline, boost::noncopyable {
public:
    VulkanComputePipeline(VulkanDevice &device, VulkanDescriptors &descriptors,
                          VulkanUniformRing &uniformRing,
                          const ComputePipelineDesc &desc,
                          std::vector<uint32_t> spirv, ShaderReflection reflection);

    void init();
    std::vector<ComputeResourceSlot> resolveBindings(
        std::initializer_list<const char *> names) const override;
    const std::vector<ShaderBindingDescription> &bindings() const override {
        return _bindings;
    }
    VkPipeline handle() const { return _pipeline.handle(); }
    void dispatch(VkCommandBuffer commandBuffer, uint32_t frameIndex, glm::uvec3 groups,
                  const ComputeBindingSet &bindings,
                  const ComputeBindingSet *overrides,
                  const void *pushConstants, uint32_t pushConstantSize);

private:
    VulkanDevice &_device;
    VulkanDescriptors &_descriptors;
    VulkanUniformRing &_uniformRing;
    ComputePipelineDesc _desc;
    std::vector<uint32_t> _spirv;
    std::vector<ShaderBindingDescription> _bindings;
    std::vector<const ComputeBinding *> _resolvedBindings;
    std::optional<uint32_t> _frameUniformSet;
    uint32_t _pushConstantSize {0};
    VulkanPipeline _pipeline;
};

VulkanComputePipeline &toVulkanComputePipeline(IComputePipeline &pipeline);

} // namespace graphics

} // namespace reone
