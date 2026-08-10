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

#include "reone/graphics/vulkan/pipeline.h"

#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/commandbuffer.h"
#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/uniformring.h"

#include "reone/graphics/vulkan/device.h"

#include "reone/graphics/uniforms.h"

#include <array>

namespace reone {

namespace graphics {

namespace {

const char *resourceKindName(ShaderResourceKind kind) {
    switch (kind) {
    case ShaderResourceKind::SampledImage: return "sampled image";
    case ShaderResourceKind::CombinedImageSampler: return "combined image sampler";
    case ShaderResourceKind::StorageImage: return "storage image";
    case ShaderResourceKind::StorageBuffer: return "storage buffer";
    case ShaderResourceKind::UniformBuffer: return "uniform buffer";
    case ShaderResourceKind::Sampler: return "sampler";
    case ShaderResourceKind::AccelerationStructure: return "acceleration structure";
    }
    return "unknown";
}

/**
 * The kinds a compute shader may declare, and nothing else. The layout and the
 * descriptor write have to agree exactly, so they read the answer from here
 * rather than each deciding it - they disagreed once, and a storage image
 * written into a combined-sampler slot is silent until the validation layers
 * are on.
 */
VkDescriptorType descriptorTypeFor(ShaderResourceKind kind) {
    switch (kind) {
    case ShaderResourceKind::StorageImage:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case ShaderResourceKind::StorageBuffer:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case ShaderResourceKind::CombinedImageSampler:
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    default:
        throw std::runtime_error(std::string("Vulkan: compute shaders cannot bind a ") +
                                 resourceKindName(kind));
    }
}

} // namespace

namespace {

/**
 * The entry points an SPIR-V module actually declares.
 *
 * Slang names a module's only entry point "main" in the binary while keeping
 * the authored names when a module has several - so a shader with one kernel
 * silently stops answering to the name it was written with, and Vulkan
 * reports that as an opaque pipeline-creation failure. Reading the names lets
 * the mismatch be resolved where it is unambiguous and named where it is not.
 */
std::vector<std::string> spirvEntryPoints(const std::vector<uint32_t> &words) {
    std::vector<std::string> names;
    if (words.size() < 5 || words[0] != 0x07230203u)
        return names;
    for (size_t i = 5; i < words.size();) {
        const uint32_t count = words[i] >> 16;
        const uint32_t opcode = words[i] & 0xFFFFu;
        if (count == 0 || i + count > words.size())
            break;
        // OpEntryPoint: execution model, id, then the name as literal words.
        if (opcode == 15u && count > 3) {
            // The literal string runs from word 3 until its NUL; the words
            // after it are interface ids, so the terminator has to end the
            // whole scan and not just the word it sits in.
            std::string name;
            bool terminated = false;
            for (size_t w = i + 3; w < i + count && !terminated; ++w) {
                const uint32_t word = words[w];
                for (int b = 0; b < 4; ++b) {
                    const char c = static_cast<char>((word >> (8 * b)) & 0xFFu);
                    if (c == char {0}) {
                        terminated = true;
                        break;
                    }
                    name.push_back(c);
                }
            }
            names.push_back(std::move(name));
        }
        i += count;
    }
    return names;
}

} // namespace

void VulkanPipeline::init(const Config &config) {
    deinit();
    try {
    _setLayouts.reserve(config.descriptorSets.size());
    _sets.resize(config.descriptorSets.size());
    for (const auto &set : config.descriptorSets) {
        if (set.layout != VK_NULL_HANDLE) {
            if (!set.bindings.empty() || set.copies != 0)
                throw std::invalid_argument("Vulkan: borrowed descriptor layout has builder state");
            _setLayouts.push_back(set.layout);
            continue;
        }
        if (set.bindings.empty()) {
            if (set.copies != 0)
                throw std::invalid_argument("Vulkan: empty descriptor layout owns no sets");
            VkDescriptorSetLayoutCreateInfo layoutInfo {
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            VkDescriptorSetLayout layout {VK_NULL_HANDLE};
            if (vkCreateDescriptorSetLayout(_device.handle(), &layoutInfo, nullptr, &layout) != VK_SUCCESS)
                throw std::runtime_error("Vulkan: empty descriptor layout creation failed");
            _setLayouts.push_back(layout);
            _ownedSets.push_back({layout});
            continue;
        }
        if (set.copies == 0)
            throw std::invalid_argument("Vulkan: owned descriptor layout needs copies");
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorBindingFlags> bindingFlags;
        bindings.reserve(set.bindings.size());
        bindingFlags.reserve(set.bindings.size());
        for (const auto &source : set.bindings) {
            VkDescriptorSetLayoutBinding binding {};
            binding.binding = source.binding.index;
            binding.descriptorType = source.binding.type;
            binding.descriptorCount = source.count;
            binding.stageFlags = source.stages;
            bindings.push_back(binding);
            bindingFlags.push_back(source.flags);
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bindingFlagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
        bindingFlagsInfo.pBindingFlags = bindingFlags.data();
        VkDescriptorSetLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.pNext = std::any_of(bindingFlags.begin(), bindingFlags.end(),
                                      [](auto flags) { return flags != 0; }) ? &bindingFlagsInfo : nullptr;
        layoutInfo.flags = set.flags;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        VkDescriptorSetLayout layout {VK_NULL_HANDLE};
        if (vkCreateDescriptorSetLayout(_device.handle(), &layoutInfo, nullptr, &layout) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: descriptor layout creation failed");
        _setLayouts.push_back(layout);
        _ownedSets.push_back({layout});

        std::vector<VkDescriptorPoolSize> poolSizes;
        for (const auto &binding : set.bindings) {
            auto poolSize = std::find_if(poolSizes.begin(), poolSizes.end(), [&](const auto &candidate) {
                return candidate.type == binding.binding.type;
            });
            if (poolSize == poolSizes.end()) {
                poolSizes.push_back({binding.binding.type, binding.count * set.copies});
            } else {
                poolSize->descriptorCount += binding.count * set.copies;
            }
        }
        VkDescriptorPoolCreateInfo poolInfo {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.flags = set.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT
                             ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT
                             : 0;
        poolInfo.maxSets = set.copies;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        VkDescriptorPool pool {VK_NULL_HANDLE};
        if (vkCreateDescriptorPool(_device.handle(), &poolInfo, nullptr, &pool) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: descriptor pool creation failed");
        _ownedSets.back().pool = pool;
        auto &sets = _sets[_setLayouts.size() - 1];
        sets.resize(set.copies);
        std::vector<VkDescriptorSetLayout> layouts(set.copies, layout);
        VkDescriptorSetAllocateInfo allocation {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = pool;
        allocation.descriptorSetCount = set.copies;
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(_device.handle(), &allocation, sets.data()) != VK_SUCCESS)
            throw std::runtime_error("Vulkan: descriptor set allocation failed");
    }
    VkShaderModuleCreateInfo moduleInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = config.spirv.size() * sizeof(uint32_t);
    moduleInfo.pCode = config.spirv.data();

    // Both graphics stages come from the same module. Slang emits one SPIR-V blob with
    // every entry point compiled into it, which is also how the shared code in
    // slang/lib is only compiled once.
    VkShaderModule module {VK_NULL_HANDLE};
    if (vkCreateShaderModule(_device.handle(), &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: shader module creation failed");
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = module;
    stages[0].pName = config.vertexEntry.c_str();
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = module;
    stages[1].pName = config.fragmentEntry.c_str();

    VkPushConstantRange singlePushConstant {};
    std::vector<VkPushConstantRange> pushConstants = config.pushConstants;
    if (config.pushConstantSize != 0) {
        if (!pushConstants.empty())
            throw std::invalid_argument("Vulkan: pipeline has two push-constant descriptions");
        singlePushConstant.offset = 0;
        singlePushConstant.size = config.pushConstantSize;
        singlePushConstant.stageFlags = config.type == Config::Type::Compute
            ? VK_SHADER_STAGE_COMPUTE_BIT
            : config.type == Config::Type::RayTracing
                  ? VK_SHADER_STAGE_RAYGEN_BIT_KHR
                  : VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstants.push_back(singlePushConstant);
    }
    VkPipelineLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = static_cast<uint32_t>(_setLayouts.size());
    layoutInfo.pSetLayouts = _setLayouts.data();
    layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
    layoutInfo.pPushConstantRanges = pushConstants.data();
    if (vkCreatePipelineLayout(_device.handle(), &layoutInfo, nullptr, &_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(_device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: pipeline layout creation failed");
    }

    if (config.type == Config::Type::Compute) {
        VkComputePipelineCreateInfo pipelineInfo {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stages[0];
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        // Resolve the entry name against what the module declares. One
        // unambiguous candidate is used whatever it is called; anything else
        // is named rather than left to Vulkan's opaque failure.
        std::string entry = config.computeEntry;
        const auto declared = spirvEntryPoints(config.spirv);
        if (!declared.empty() &&
            std::find(declared.begin(), declared.end(), entry) == declared.end()) {
            if (declared.size() == 1) {
                entry = declared.front();
            } else {
                std::string available;
                for (const auto &name : declared)
                    available += (available.empty() ? "" : ", ") + name;
                vkDestroyShaderModule(_device.handle(), module, nullptr);
                throw std::runtime_error("Vulkan: compute entry '" + config.computeEntry +
                                         "' not in module; it declares: " + available);
            }
        }
        pipelineInfo.stage.pName = entry.c_str();
        pipelineInfo.layout = _layout;
        const auto result = vkCreateComputePipelines(_device.handle(), VK_NULL_HANDLE, 1,
                                                     &pipelineInfo, nullptr, &_pipeline);
        vkDestroyShaderModule(_device.handle(), module, nullptr);
        if (result != VK_SUCCESS)
            throw std::runtime_error("Vulkan: compute pipeline creation failed");
        return;
    }
    if (config.type == Config::Type::RayTracing) {
        VkPipelineShaderStageCreateInfo stage = stages[0];
        stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        stage.pName = config.raygenEntry.c_str();
        VkRayTracingShaderGroupCreateInfoKHR group {
            VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        group.generalShader = 0;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
        VkRayTracingPipelineCreateInfoKHR pipelineInfo {
            VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        pipelineInfo.stageCount = 1;
        pipelineInfo.pStages = &stage;
        pipelineInfo.groupCount = 1;
        pipelineInfo.pGroups = &group;
        pipelineInfo.maxPipelineRayRecursionDepth = 1;
        pipelineInfo.layout = _layout;
        const auto result = vkCreateRayTracingPipelinesKHR(
            _device.handle(), VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_pipeline);
        vkDestroyShaderModule(_device.handle(), module, nullptr);
        if (result != VK_SUCCESS)
            throw std::runtime_error("Vulkan: ray-tracing pipeline creation failed");
        return;
    }
    VkPipelineVertexInputStateCreateInfo vertexInput {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount =
        static_cast<uint32_t>(config.vertexBindings.size());
    vertexInput.pVertexBindingDescriptions = config.vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(config.vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions = config.vertexAttributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    switch (config.cull) {
    case FaceCullMode::Front:
        raster.cullMode = VK_CULL_MODE_FRONT_BIT;
        break;
    case FaceCullMode::Back:
        raster.cullMode = VK_CULL_MODE_BACK_BIT;
        break;
    default:
        raster.cullMode = VK_CULL_MODE_NONE;
        break;
    }
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    raster.depthBiasEnable = config.depthBias ? VK_TRUE : VK_FALSE;
    raster.depthBiasConstantFactor = config.depthBiasConstantFactor;
    raster.depthBiasSlopeFactor = config.depthBiasSlopeFactor;

    VkPipelineMultisampleStateCreateInfo multisample {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // One blend state per attachment is mandatory, even when they are all the
    // same: a count mismatch here is a validation error, not a default.
    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
        config.colorFormats.size());
    for (auto &attachment : blendAttachments) {
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        switch (config.blend) {
        case BlendMode::Normal:
            attachment.blendEnable = VK_TRUE;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BlendMode::Premultiplied:
            attachment.blendEnable = VK_TRUE;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BlendMode::Additive:
            attachment.blendEnable = VK_TRUE;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        case BlendMode::OIT_Transparent:
            // Weighted-blended transparency, matching the glBlendFuncSeparate
            // in context.cpp. Colour accumulates additively; alpha multiplies
            // down by one minus coverage, which is what turns the first
            // attachment's alpha into revealage given a clear of 1.0.
            //
            // The loop puts this on every attachment, as OpenGL's single blend
            // state does - so the OIT pass needs no independentBlend feature.
            attachment.blendEnable = VK_TRUE;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            break;
        default:
            attachment.blendEnable = VK_FALSE;
            break;
        }
    }

    VkPipelineColorBlendStateCreateInfo blend {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    blend.pAttachments = blendAttachments.data();

    VkPipelineDepthStencilStateCreateInfo depthStencil {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (config.depthFormat != VK_FORMAT_UNDEFINED) {
        depthStencil.depthTestEnable = config.depthTest ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = config.depthWrite ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencil.maxDepthBounds = 1.0f;
    }

    std::array<VkDynamicState, 2> dynamicStates {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();

    // Dynamic rendering: no VkRenderPass and no VkFramebuffer, just the formats
    // the pipeline will write. Fewer objects to keep in step as passes change.
    VkPipelineRenderingCreateInfo rendering {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.viewMask = config.viewMask;
    rendering.colorAttachmentCount = static_cast<uint32_t>(config.colorFormats.size());
    rendering.pColorAttachmentFormats = config.colorFormats.data();
    rendering.depthAttachmentFormat = config.depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = _layout;

    auto result = vkCreateGraphicsPipelines(_device.handle(), VK_NULL_HANDLE, 1,
                                            &pipelineInfo, nullptr, &_pipeline);
    // The module can go as soon as the pipeline is built; the pipeline holds
    // whatever it needs from it.
    vkDestroyShaderModule(_device.handle(), module, nullptr);
    if (result != VK_SUCCESS)
        throw std::runtime_error("Vulkan: graphics pipeline creation failed");
    } catch (...) {
        deinit();
        throw;
    }
}

void VulkanPipeline::deinit() {
    if (_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(_device.handle(), _pipeline, nullptr);
        _pipeline = VK_NULL_HANDLE;
    }
    if (_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(_device.handle(), _layout, nullptr);
        _layout = VK_NULL_HANDLE;
    }
    for (const auto &set : _ownedSets) {
        if (set.pool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(_device.handle(), set.pool, nullptr);
        vkDestroyDescriptorSetLayout(_device.handle(), set.layout, nullptr);
    }
    _ownedSets.clear();
    _setLayouts.clear();
    _sets.clear();
}

VkDescriptorSet VulkanPipeline::descriptorSet(uint32_t set, uint32_t copy) const {
    if (set >= _sets.size() || copy >= _sets[set].size())
        throw std::out_of_range("Vulkan: descriptor set copy is not owned by this pipeline");
    return _sets[set][copy];
}

VulkanComputePipeline::VulkanComputePipeline(
    VulkanDevice &device, VulkanDescriptors &descriptors, VulkanUniformRing &uniformRing,
    const ComputePipelineDesc &desc, std::vector<uint32_t> spirv,
    ShaderReflection reflection) :
    _device(device), _descriptors(descriptors), _uniformRing(uniformRing), _desc(desc), _spirv(std::move(spirv)),
    _bindings(std::move(reflection.bindings)),
    _pushConstantSize(reflection.pushConstantSize), _pipeline(device) {}

VulkanComputePipeline &toVulkanComputePipeline(IComputePipeline &pipeline) {
    auto *result = dynamic_cast<VulkanComputePipeline *>(&pipeline);
    if (!result)
        throw std::invalid_argument("Compute pipeline is not implemented by Vulkan");
    return *result;
}

void VulkanComputePipeline::init() {
    uint32_t maxSet = 0;
    for (const auto &binding : _bindings)
        maxSet = std::max(maxSet, binding.set);
    std::vector<VulkanPipeline::DescriptorSet> sets(maxSet + 1);
    std::vector<ShaderBindingDescription> dispatchBindings;
    dispatchBindings.reserve(_bindings.size());
    for (const auto &binding : _bindings) {
        if (binding.kind == ShaderResourceKind::UniformBuffer) {
            if (binding.set != VulkanDescriptors::kUniformSet ||
                binding.binding >= VulkanDescriptors::kNumUniformBlocks || binding.count != 1)
                throw std::runtime_error("Vulkan: compute shader '" + _desc.shader +
                                         "' has a non-frame uniform binding '" + binding.name + "'");
            if (!sets[binding.set].bindings.empty()) {
                std::string occupants;
                for (const auto &other : dispatchBindings)
                    if (other.set == binding.set)
                        occupants += " '" + other.name + "'@" + std::to_string(other.binding);
                throw std::runtime_error("Vulkan: compute shader '" + _desc.shader +
                                         "' mixes frame uniforms and resources in descriptor set " +
                                         std::to_string(binding.set) + "; uniform '" + binding.name +
                                         "' arrived after:" + occupants);
            }
            if (_frameUniformSet && *_frameUniformSet != binding.set)
                throw std::runtime_error("Vulkan: compute shader '" + _desc.shader +
                                         "' spreads frame uniforms across descriptor sets");
            _frameUniformSet = binding.set;
            continue;
        }
        if (_frameUniformSet && binding.set == *_frameUniformSet)
            throw std::runtime_error("Vulkan: compute shader '" + _desc.shader +
                                     "' mixes frame uniforms and resources in descriptor set " +
                                     std::to_string(binding.set) + ": '" + binding.name +
                                     "' (kind " + std::to_string(static_cast<int>(binding.kind)) +
                                     ", binding " + std::to_string(binding.binding) + ")");
        // A Sampler2D rather than an RWTexture2D is how a pass asks for the
        // filtering unit: same image, same call site, hardware interpolation
        // instead of taps in the shader.
        VkDescriptorType descriptorType;
        try {
            descriptorType = descriptorTypeFor(binding.kind);
        } catch (const std::runtime_error &) {
            throw std::runtime_error("Vulkan: unsupported compute binding '" + binding.name +
                                     "' in shader '" + _desc.shader + "': " +
                                     resourceKindName(binding.kind));
        }
        sets[binding.set].bindings.push_back({{binding.binding, descriptorType}, binding.count,
                                               VK_SHADER_STAGE_COMPUTE_BIT});
        sets[binding.set].copies = _desc.descriptorSetCopies;
        dispatchBindings.push_back(binding);
    }
    if (_frameUniformSet)
        sets[*_frameUniformSet].layout = _descriptors.uniformLayout();
    _bindings = std::move(dispatchBindings);
    VulkanPipeline::Config config;
    config.type = VulkanPipeline::Config::Type::Compute;
    config.spirv = std::move(_spirv);
    config.computeEntry = _desc.entry;
    config.descriptorSets = std::move(sets);
    config.pushConstantSize = _pushConstantSize;
    _pipeline.init(config);
    _resolvedBindings.resize(_bindings.size());
}

std::vector<ComputeResourceSlot> VulkanComputePipeline::resolveBindings(
    std::initializer_list<const char *> names) const {
    std::vector<ComputeResourceSlot> result;
    result.reserve(names.size());
    for (const auto *name : names) {
        auto found = std::find_if(_bindings.begin(), _bindings.end(), [name](const auto &binding) {
            return binding.name == name;
        });
        if (found == _bindings.end())
            throw std::invalid_argument("Compute shader '" + _desc.shader +
                                        "' does not declare binding '" + name + "'");
        const auto slot = static_cast<uint32_t>(std::distance(_bindings.begin(), found));
        if (std::find_if(result.begin(), result.end(), [slot](const auto candidate) {
                return candidate.value == slot;
            }) != result.end())
            throw std::invalid_argument("Compute shader '" + _desc.shader +
                                        "' resolves binding '" + name + "' more than once");
        result.push_back({slot});
    }
    return result;
}

void VulkanComputePipeline::dispatch(VkCommandBuffer commandBuffer, uint32_t frameIndex,
                                     glm::uvec3 groups,
                                     const ComputeBindingSet &bindings,
                                     const ComputeBindingSet *overrides,
                                     const void *pushConstants,
                                     uint32_t pushConstantSize) {
    DescriptorWriteBuilder writes(_device.handle());
    std::fill(_resolvedBindings.begin(), _resolvedBindings.end(), nullptr);
    const auto accept = [this](const ComputeBindingSet &source,
                                          bool replace) {
        if (source.count != 0 && !source.bindings)
            throw std::invalid_argument("Compute shader '" + _desc.shader +
                                        "' received an empty binding scope");
        for (uint32_t i = 0; i < source.count; ++i) {
            const auto &binding = source.bindings[i];
            if (binding.slot.value >= _bindings.size())
                throw std::invalid_argument("Compute shader '" + _desc.shader +
                                            "' received an unknown resolved binding");
            const auto slot = binding.slot.value;
            if (_resolvedBindings[slot] && !replace)
                throw std::invalid_argument("Compute shader '" + _desc.shader + "' binding '" +
                                            _bindings[slot].name + "' was supplied twice in its scope");
            _resolvedBindings[slot] = &binding;
        }
    };
    accept(bindings, false);
    if (overrides) {
        accept(*overrides, true);
        for (uint32_t i = 0; i < overrides->count; ++i) {
            const auto slot = overrides->bindings[i].slot.value;
            for (uint32_t earlier = 0; earlier < i; ++earlier) {
                if (overrides->bindings[earlier].slot.value == slot)
                    throw std::invalid_argument("Compute shader '" + _desc.shader + "' binding '" +
                                                _bindings[slot].name + "' was overridden twice");
            }
        }
    }
    for (size_t bindingIndex = 0; bindingIndex < _bindings.size(); ++bindingIndex) {
        const auto &binding = _bindings[bindingIndex];
        const auto *source = _resolvedBindings[bindingIndex];
        if (!source)
            throw std::invalid_argument("Compute shader '" + _desc.shader + "' binding '" +
                                        binding.name + "' (set " + std::to_string(binding.set) +
                                        ", binding " + std::to_string(binding.binding) +
                                        ") is not satisfied");
        if (source->count != binding.count)
            throw std::invalid_argument("Compute shader '" + _desc.shader + "' binding '" +
                                        binding.name + "' needs " + std::to_string(binding.count) +
                                        " resources, got " + std::to_string(source->count));
        const auto set = _pipeline.descriptorSet(binding.set, frameIndex);
        const DescriptorBinding target {binding.binding,
            descriptorTypeFor(binding.kind)};
        for (uint32_t i = 0; i < source->count; ++i) {
            if (binding.kind == ShaderResourceKind::StorageImage) {
                if (source->type != ComputeBinding::Type::Image)
                    throw std::invalid_argument("Compute shader '" + _desc.shader + "' binding '" +
                                                binding.name + "' requires images");
                writes.writeStorageImage(toDescriptorSet(set), target,
                                         source->imageArray ? source->imageArray[i] : source->image, i);
            } else if (binding.kind == ShaderResourceKind::CombinedImageSampler) {
                if (source->type != ComputeBinding::Type::Image)
                    throw std::invalid_argument("Compute shader '" + _desc.shader + "' binding '" +
                                                binding.name + "' requires images");
                // General, not shader-read-only. These are the same images other
                // passes write as storage, and they are left in General for the
                // frame rather than transitioned around each read; General is a
                // legal layout to sample from, and a barrier pair per pass would
                // buy nothing here.
                writes.writeImage(set, target,
                                  {_descriptors.clampSampler(),
                                   toVulkanImageView(source->imageArray ? source->imageArray[i]
                                                                        : source->image),
                                   VK_IMAGE_LAYOUT_GENERAL},
                                  i);
            } else if (binding.kind == ShaderResourceKind::StorageBuffer) {
                if (source->type != ComputeBinding::Type::Buffer)
                    throw std::invalid_argument("Compute shader '" + _desc.shader + "' binding '" +
                                                binding.name + "' requires buffers");
                const auto &view = source->bufferArray ? source->bufferArray[i] : source->buffer;
                if (!view.buffer)
                    throw std::invalid_argument("Compute shader '" + _desc.shader + "' binding '" +
                                                binding.name + "' has no buffer");
                writes.writeBuffer(set, target, {toVulkanBuffer(*view.buffer).handle(), view.offset,
                                                  view.size}, i);
            } else {
                throw std::invalid_argument("Compute shader '" + _desc.shader + "' binding '" +
                                            binding.name + "' has an unsupported resource kind");
            }
        }
    }
    writes.apply();
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline.handle());
    if (_frameUniformSet) {
        std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = _uniformRing.globalsOffset();
        const auto uniformSet = _descriptors.uniformSet(frameIndex);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline.layout(),
                                *_frameUniformSet, 1, &uniformSet,
                                static_cast<uint32_t>(offsets.size()), offsets.data());
    }
    uint32_t maxSet = 0;
    for (const auto &binding : _bindings)
        maxSet = std::max(maxSet, binding.set);
    for (uint32_t setIndex = 0; setIndex <= maxSet; ++setIndex) {
        const auto &setBindings = std::find_if(_bindings.begin(), _bindings.end(), [setIndex](const auto &binding) {
            return binding.set == setIndex;
        });
        if (setBindings == _bindings.end())
            continue;
        const auto set = _pipeline.descriptorSet(setIndex, frameIndex);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline.layout(),
                                setIndex, 1, &set, 0, nullptr);
    }
    if (pushConstantSize != _pushConstantSize)
        throw std::invalid_argument("Compute shader '" + _desc.shader +
                                    "' push constants do not match reflected size " +
                                    std::to_string(_pushConstantSize));
    if (pushConstantSize != 0) {
        if (!pushConstants)
            throw std::invalid_argument("Compute shader '" + _desc.shader +
                                        "' push constants are null");
        vkCmdPushConstants(commandBuffer, _pipeline.layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           pushConstantSize, pushConstants);
    }
    vkCmdDispatch(commandBuffer, groups.x, groups.y, groups.z);
}

} // namespace graphics

} // namespace reone
