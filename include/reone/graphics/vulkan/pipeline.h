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

#include "reone/graphics/types.h"

namespace reone {

namespace graphics {

class VulkanDevice;

/**
 * A graphics pipeline and the layout it is built against.
 *
 * Vulkan bakes what OpenGL kept as mutable state - blend mode, depth test, cull
 * mode, the shaders themselves - into an immutable object chosen at draw time.
 * That is the reason IContext must not be implemented in Vulkan (§1.3 of the
 * plan): emulating a state machine here means hashing a state vector per draw
 * to find a pipeline.
 *
 * Viewport and scissor are left dynamic, because those genuinely do change per
 * frame and are cheap to set.
 */
class VulkanPipeline : boost::noncopyable {
public:
    struct Config {
        std::vector<uint32_t> spirv;
        std::string vertexEntry;
        std::string fragmentEntry;
        /**
         * Colour attachment formats, in attachment order, for dynamic
         * rendering. One entry for a normal pass, several for a G-buffer.
         */
        std::vector<VkFormat> colorFormats;
        /** UNDEFINED means no depth attachment. */
        VkFormat depthFormat {VK_FORMAT_UNDEFINED};
        /**
         * Which views a draw broadcasts to, zero for none. Must match the
         * VkRenderingInfo it is used with; a mismatch is a validation error.
         */
        uint32_t viewMask {0};

        // State OpenGL would have set per draw. In Vulkan it is baked in, which
        // is why these belong to the pipeline's identity rather than to a call.
        BlendMode blend {BlendMode::None};
        FaceCullMode cull {FaceCullMode::None};
        bool depthTest {false};
        bool depthWrite {false};
        std::vector<VkDescriptorSetLayout> setLayouts;
        /** Fragment push constants shared by graphics layouts (mega-draw uses
            two uints for triangle base and material-gated range selection). */
        uint32_t fragmentPushConstantSize {0};

        /**
         * Empty means the vertex shader synthesises its own geometry from
         * SV_VertexID and no vertex buffer is bound.
         */
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

private:
    VulkanDevice &_device;

    VkPipeline _pipeline {VK_NULL_HANDLE};
    VkPipelineLayout _layout {VK_NULL_HANDLE};
};

/** Read a .spv file into the word vector VkShaderModuleCreateInfo wants. */
std::vector<uint32_t> readSpirV(const std::filesystem::path &path);

} // namespace graphics

} // namespace reone
