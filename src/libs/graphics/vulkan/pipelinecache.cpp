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

#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/mesh.h"
#include "reone/graphics/vulkan/tracingstructure.h"

#include "reone/graphics/vulkan/rhi.h"

#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/descriptors.h"
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
    std::function<std::vector<uint32_t>(const std::string &)> moduleLoader) {
    _moduleLoader = std::move(moduleLoader);
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
    config.descriptorSets = {{_descriptors.uniformLayout()},
                             {_descriptors.textureLayout()},
                             {_descriptors.megaDrawLayout()}};
    // Every cached graphics layout exposes the same tiny fragment range. This
    // keeps layouts shared by sky/resolve valid while allowing the mega-draw
    // shader to select the global triangle range without a per-draw buffer.
    config.pushConstants = {{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 2 * sizeof(uint32_t)}};
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

PipelineBinding VulkanPipelineCache::get(const PipelineKey &key) {
    Key nativeKey;
    nativeKey.module = key.module;
    nativeKey.vertexEntry = key.vertexEntry;
    nativeKey.fragmentEntry = key.fragmentEntry;
    nativeKey.viewMask = key.viewMask;
    nativeKey.blend = key.blend;
    nativeKey.depthTest = key.depthTest;
    nativeKey.depthWrite = key.depthWrite;
    nativeKey.depthFormat = toVulkanFormat(key.depthFormat);
    nativeKey.depthBias = key.depthBias;
    nativeKey.depthBiasConstantFactor = key.depthBiasConstantFactor;
    nativeKey.depthBiasSlopeFactor = key.depthBiasSlopeFactor;
    nativeKey.cull = key.cull;
    nativeKey.colorFormats.reserve(key.colorFormats.size());
    for (auto format : key.colorFormats) {
        nativeKey.colorFormats.push_back(toVulkanFormat(format));
    }
    if (key.vertexLayout) {
        nativeKey.vertexBindings = VulkanMesh::bindingDescriptions(*key.vertexLayout);
        nativeKey.vertexAttributes = VulkanMesh::attributeDescriptions(*key.vertexLayout);
    }
    auto &pipeline = get(nativeKey);
    return {toPipeline(pipeline.handle()), toPipelineLayout(pipeline.layout())};
}

namespace {

class VulkanRayTracingPipeline : public ITracingPipeline {
public:
    VulkanRayTracingPipeline(VulkanDevice &device, std::unique_ptr<VulkanPipeline> pipeline,
                             std::unordered_map<std::string, DescriptorBinding> bindings,
                             uint32_t bindlessTextureCapacity) :
        _device(device), _pipeline(std::move(pipeline)), _bindings(std::move(bindings)),
        _bindlessTextureCapacity(bindlessTextureCapacity) {}

    Pipeline pipeline() const override { return _pipeline->pipeline(); }
    PipelineLayout pipelineLayout() const override { return _pipeline->pipelineLayout(); }
    DescriptorSet descriptorSet(uint32_t set, uint32_t frameIndex) const override {
        return _pipeline->descriptorSetHandle(set, frameIndex);
    }
    uint32_t bindlessTextureCapacity() const override { return _bindlessTextureCapacity; }

    void updateBindings(uint32_t set, uint32_t frameIndex,
                        const TracingBindingSet &bindings) override {
        DescriptorWriteBuilder writes(_device.handle());
        const auto descriptorSet = _pipeline->descriptorSet(set, frameIndex);
        for (uint32_t i = 0; i < bindings.count; ++i) {
            const auto &source = bindings.bindings[i];
            const auto &target = binding(source.name);
            if (source.structure) {
                writes.writeAccelerationStructure(descriptorSet, target,
                                                  toVulkanTracingStructure(*source.structure).handle());
            } else if (source.buffer.buffer) {
                writes.writeBuffer(descriptorSet, target,
                                  {toVulkanBuffer(*source.buffer.buffer).handle(), source.buffer.offset,
                                   source.buffer.size});
            } else if (source.images) {
                for (uint32_t index = 0; index < source.imageCount; ++index) {
                    if (!source.images[index]) {
                        continue;
                    }
                    const auto &image = toVulkanImage(*source.images[index]);
                    writes.writeSampledImage(toDescriptorSet(descriptorSet), target,
                                             image.sampleSampler(), image.sampleView(), index);
                }
            } else if (source.image) {
                const auto &image = toVulkanImage(*source.image);
                const auto imageView = source.hasImageView ? source.imageView : image.sampleView();
                if (target.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                    writes.writeStorageImage(toDescriptorSet(descriptorSet), target, imageView);
                } else {
                    writes.writeSampledImage(toDescriptorSet(descriptorSet), target,
                                             image.sampleSampler(), imageView,
                                             source.arrayElement ? source.arrayIndex : 0);
                }
            } else {
                throw std::invalid_argument("Vulkan: ray-query binding has no resource");
            }
        }
        writes.apply();
    }

private:
    const DescriptorBinding &binding(const char *name) const {
        const auto found = _bindings.find(name);
        if (found == _bindings.end()) {
            throw std::runtime_error("Vulkan: ray-query does not declare binding '" +
                                     std::string(name) + "'");
        }
        return found->second;
    }

    VulkanDevice &_device;
    std::unique_ptr<VulkanPipeline> _pipeline;
    std::unordered_map<std::string, DescriptorBinding> _bindings;
    uint32_t _bindlessTextureCapacity {0};
};

} // namespace

std::unique_ptr<ITracingPipeline> VulkanPipelineCache::makeTracingPipeline(
    const std::vector<uint32_t> &spirv, const ShaderReflection &reflection,
    uint32_t bindlessTextureCapacity, uint32_t pushConstantSize, const std::string &label) {
    if (bindlessTextureCapacity == 0)
        throw std::runtime_error("Vulkan: ray-query bindless texture capacity is zero");
    if (reflection.stage != ShaderStage::Unknown && reflection.stage != ShaderStage::RayGeneration)
        throw std::runtime_error("Vulkan: ray-query entry point is not a ray-generation shader");

    std::unordered_map<std::string, DescriptorBinding> reflectedBindings;
    std::vector<VulkanPipeline::LayoutBinding> bindings;
    std::vector<VulkanPipeline::LayoutBinding> auxBindings;
    for (const auto &reflected : reflection.bindings) {
        if (reflected.set == 0) {
            if (reflected.kind != ShaderResourceKind::UniformBuffer || reflected.count != 1)
                throw std::runtime_error("Vulkan: ray-query has an unsupported frame binding '" +
                                         reflected.name + "'");
            continue;
        }
        if (reflected.set != 1 && reflected.set != 2)
            throw std::runtime_error("Vulkan: ray-query declares binding '" + reflected.name +
                                     "' in unsupported descriptor set " + std::to_string(reflected.set));
        const bool bindless = reflected.count == 0;
        if (bindless && reflected.kind != ShaderResourceKind::CombinedImageSampler)
            throw std::runtime_error("Vulkan: ray-query unbounded binding '" + reflected.name +
                                     "' is not a sampled-image array");
        VkDescriptorType type;
        switch (reflected.kind) {
        case ShaderResourceKind::StorageImage: type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; break;
        case ShaderResourceKind::StorageBuffer: type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
        case ShaderResourceKind::CombinedImageSampler: type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
        case ShaderResourceKind::AccelerationStructure: type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; break;
        default: throw std::runtime_error("Vulkan: ray-query reflection has an unsupported descriptor kind");
        }
        const DescriptorBinding target {reflected.binding, type};
        if (!reflectedBindings.emplace(reflected.name, target).second)
            throw std::runtime_error("Vulkan: ray-query reflects binding '" + reflected.name + "' twice");
        auto &setBindings = reflected.set == 1 ? bindings : auxBindings;
        setBindings.push_back({target, bindless ? bindlessTextureCapacity : reflected.count,
                               VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                               bindless ? static_cast<VkDescriptorBindingFlags>(
                                              VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                              VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) : 0});
    }
    if (bindings.empty() || auxBindings.empty())
        throw std::runtime_error("Vulkan: ray-query reflection omitted a resource descriptor set");

    VulkanPipeline::Config config;
    config.type = VulkanPipeline::Config::Type::RayTracing;
    config.spirv = spirv;
    config.raygenEntry = "main";
    config.descriptorSets = {{_descriptors.uniformLayout()},
                             {VK_NULL_HANDLE, std::move(bindings), 2,
                              VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT},
                             {VK_NULL_HANDLE, std::move(auxBindings), 2}};
    config.pushConstantSize = pushConstantSize;
    auto pipeline = std::make_unique<VulkanPipeline>(_device);
    pipeline->init(config);
    _device.setObjectName(VK_OBJECT_TYPE_PIPELINE,
                          reinterpret_cast<uint64_t>(pipeline->handle()), label);
    return std::make_unique<VulkanRayTracingPipeline>(
        _device, std::move(pipeline), std::move(reflectedBindings), bindlessTextureCapacity);
}

} // namespace graphics

} // namespace reone
