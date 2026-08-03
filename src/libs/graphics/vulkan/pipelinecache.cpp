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

#include "reone/graphics/vulkan/pipelinecache.h"

#include "reone/graphics/vulkan/device.h"
#include "reone/system/logutil.h"

namespace reone {

namespace graphics {

bool VulkanPipelineCache::Key::operator==(const Key &other) const {
    if (module != other.module ||
        vertexEntry != other.vertexEntry ||
        fragmentEntry != other.fragmentEntry ||
        depthFormat != other.depthFormat ||
        viewMask != other.viewMask ||
        blend != other.blend ||
        cull != other.cull ||
        depthTest != other.depthTest ||
        depthWrite != other.depthWrite ||
        depthBias != other.depthBias ||
        depthBiasConstantFactor != other.depthBiasConstantFactor ||
        depthBiasSlopeFactor != other.depthBiasSlopeFactor ||
        colorFormats != other.colorFormats ||
        vertexBindings.size() != other.vertexBindings.size() ||
        vertexAttributes.size() != other.vertexAttributes.size()) {
        return false;
    }
    // The Vulkan structs have no operator==, and comparing them bytewise is
    // safe here because both sides are value-initialised aggregates of scalars.
    for (size_t i = 0; i < vertexBindings.size(); ++i) {
        if (std::memcmp(&vertexBindings[i], &other.vertexBindings[i],
                        sizeof(VkVertexInputBindingDescription)) != 0) {
            return false;
        }
    }
    for (size_t i = 0; i < vertexAttributes.size(); ++i) {
        if (std::memcmp(&vertexAttributes[i], &other.vertexAttributes[i],
                        sizeof(VkVertexInputAttributeDescription)) != 0) {
            return false;
        }
    }
    return true;
}

size_t VulkanPipelineCache::KeyHash::operator()(const Key &key) const {
    size_t hash = std::hash<std::string> {}(key.module);
    auto mix = [&hash](size_t value) {
        hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    };
    mix(std::hash<std::string> {}(key.vertexEntry));
    mix(std::hash<std::string> {}(key.fragmentEntry));
    for (auto format : key.colorFormats) {
        mix(static_cast<size_t>(format));
    }
    mix(static_cast<size_t>(key.depthFormat));
    mix(static_cast<size_t>(key.viewMask));
    mix(key.vertexBindings.size());
    mix(key.vertexAttributes.size());
    for (const auto &attribute : key.vertexAttributes) {
        mix(attribute.location * 31u + attribute.offset);
    }
    mix(static_cast<size_t>(key.blend));
    mix(static_cast<size_t>(key.cull));
    mix(static_cast<size_t>(key.depthTest) | (static_cast<size_t>(key.depthWrite) << 1));
    mix(static_cast<size_t>(key.depthBias));
    mix(std::hash<float> {}(key.depthBiasConstantFactor));
    mix(std::hash<float> {}(key.depthBiasSlopeFactor));
    return hash;
}

void VulkanPipelineCache::init(
    std::function<std::vector<uint32_t>(const std::string &)> moduleLoader,
    std::vector<VkDescriptorSetLayout> setLayouts) {
    _moduleLoader = std::move(moduleLoader);
    _setLayouts = std::move(setLayouts);
}

void VulkanPipelineCache::deinit() {
    _pipelines.clear();
    _modules.clear();
}

VulkanPipeline &VulkanPipelineCache::get(const Key &key) {
    auto existing = _pipelines.find(key);
    if (existing != _pipelines.end()) {
        return *existing->second;
    }

    auto module = _modules.find(key.module);
    if (module == _modules.end()) {
        module = _modules.insert({key.module, _moduleLoader(key.module)}).first;
    }

    VulkanPipeline::Config config;
    config.spirv = module->second;
    config.vertexEntry = key.vertexEntry;
    config.fragmentEntry = key.fragmentEntry;
    config.colorFormats = key.colorFormats;
    config.depthFormat = key.depthFormat;
    config.viewMask = key.viewMask;
    config.vertexBindings = key.vertexBindings;
    config.vertexAttributes = key.vertexAttributes;
    config.setLayouts = _setLayouts;
    // Every cached graphics layout exposes the same tiny fragment range. This
    // keeps layouts shared by sky/resolve valid while allowing the mega-draw
    // shader to select the global triangle range without a per-draw buffer.
    config.fragmentPushConstantSize = 2 * sizeof(uint32_t);
    config.blend = key.blend;
    config.cull = key.cull;
    config.depthTest = key.depthTest;
    config.depthWrite = key.depthWrite;
    config.depthBias = key.depthBias;
    config.depthBiasConstantFactor = key.depthBiasConstantFactor;
    config.depthBiasSlopeFactor = key.depthBiasSlopeFactor;

    auto pipeline = std::make_unique<VulkanPipeline>(_device);
    pipeline->init(config);
    // Named after the shaders it was built from, so a capture says which
    // program a draw used instead of a bare handle.
    auto label = key.module + ":" + key.vertexEntry + "/" + key.fragmentEntry;
    _device.setObjectName(VK_OBJECT_TYPE_PIPELINE,
                          reinterpret_cast<uint64_t>(pipeline->handle()), label);
    _device.setObjectName(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
                          reinterpret_cast<uint64_t>(pipeline->layout()), label + " layout");
    debug(str(boost::format("Vulkan: built pipeline %s:%s/%s (%d cached)") %
              key.module % key.vertexEntry % key.fragmentEntry % (_pipelines.size() + 1)),
          LogChannel::Graphics);
    return *_pipelines.insert({key, std::move(pipeline)}).first->second;
}

} // namespace graphics

} // namespace reone
