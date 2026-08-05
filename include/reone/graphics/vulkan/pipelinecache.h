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

#include "reone/graphics/pipelinecache.h"

#include "pipeline.h"

namespace reone {

namespace graphics {

class VulkanDevice;
class VulkanDescriptors;

/**
 * Graphics pipelines, built on first use and kept.
 *
 * Vulkan bakes into a pipeline object everything OpenGL kept as mutable state,
 * so the number of pipelines is the number of distinct state combinations the
 * renderer actually uses - shader pair, attachment formats, vertex layout,
 * blend, cull, depth. That is a modest set, but it is not knowable up front,
 * and building one mid-frame costs milliseconds.
 *
 * Hence a cache rather than a fixed table: ask for the combination you want and
 * get it, built once. §1.3 of the plan warns against a per-draw state hash
 * emulating a state machine; this is the other thing, a small keyed set of
 * pipelines the renderer names deliberately.
 */
class VulkanPipelineCache : public IPipelineCache, boost::noncopyable {
public:
    /**
     * Everything that distinguishes one pipeline from another. Anything absent
     * here is fixed for every pipeline in the backend.
     */
    struct Key {
        /** Name of the SPIR-V module, as passed to the loader. */
        std::string module;
        std::string vertexEntry;
        std::string fragmentEntry;
        std::vector<VkFormat> colorFormats;
        VkFormat depthFormat {VK_FORMAT_UNDEFINED};
        /** See VulkanPipeline::Config::viewMask. */
        uint32_t viewMask {0};
        /** Empty for shaders that synthesise geometry from SV_VertexID. */
        std::vector<VkVertexInputBindingDescription> vertexBindings;
        std::vector<VkVertexInputAttributeDescription> vertexAttributes;
        BlendMode blend {BlendMode::None};
        FaceCullMode cull {FaceCullMode::None};
        bool depthTest {false};
        bool depthWrite {false};
        bool depthBias {false};
        float depthBiasConstantFactor {0.0f};
        float depthBiasSlopeFactor {0.0f};

        bool operator==(const Key &other) const;
    };

    struct KeyHash {
        size_t operator()(const Key &key) const;
    };

    VulkanPipelineCache(VulkanDevice &device, VulkanDescriptors &descriptors) :
        _device(device), _descriptors(descriptors) {
    }

    /**
     * @param moduleLoader turns a module name into SPIR-V. Called only on a
     *                     miss, so a hit costs one hash lookup.
     */
    void init(std::function<std::vector<uint32_t>(const std::string &)> moduleLoader);
    void deinit();

    /** The pipeline for @p key, built if this is the first request for it. */
    VulkanPipeline &get(const Key &key);
    PipelineBinding get(const PipelineKey &key) override;

    size_t size() const { return _pipelines.size(); }

private:
    VulkanDevice &_device;
    VulkanDescriptors &_descriptors;

    std::function<std::vector<uint32_t>(const std::string &)> _moduleLoader;

    std::unordered_map<Key, std::unique_ptr<VulkanPipeline>, KeyHash> _pipelines;
    /** SPIR-V is kept so a second entry point in the same module is free. */
    std::unordered_map<std::string, std::vector<uint32_t>> _modules;
};

} // namespace graphics

} // namespace reone
