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

#include "reone/graphics/vulkan/device.h"

namespace reone {

namespace graphics {

std::vector<uint32_t> readSpirV(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.good()) {
        throw std::runtime_error("Vulkan: cannot open SPIR-V module: " + path.string());
    }
    auto size = static_cast<size_t>(file.tellg());
    if (size % sizeof(uint32_t) != 0) {
        throw std::runtime_error("Vulkan: SPIR-V module is not a whole number of words: " +
                                 path.string());
    }
    std::vector<uint32_t> words(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(words.data()), size);
    return words;
}

void VulkanPipeline::init(const Config &config) {
    VkShaderModuleCreateInfo moduleInfo {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = config.spirv.size() * sizeof(uint32_t);
    moduleInfo.pCode = config.spirv.data();

    // Both stages come from the same module. Slang emits one SPIR-V blob with
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

    VkPipelineLayoutCreateInfo layoutInfo {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = static_cast<uint32_t>(config.setLayouts.size());
    layoutInfo.pSetLayouts = config.setLayouts.data();
    VkPushConstantRange pushRange {};
    if (config.fragmentPushConstantSize != 0) {
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.size = config.fragmentPushConstantSize;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
    }
    if (vkCreatePipelineLayout(_device.handle(), &layoutInfo, nullptr, &_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(_device.handle(), module, nullptr);
        throw std::runtime_error("Vulkan: pipeline layout creation failed");
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
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Vulkan: graphics pipeline creation failed");
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
}

} // namespace graphics

} // namespace reone
