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

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "buffer.h"
#include "rhi.h"

namespace reone::graphics {

/** Slang's API-independent description of one descriptor binding. */
enum class ShaderResourceKind {
    SampledImage,
    CombinedImageSampler,
    StorageImage,
    StorageBuffer,
    UniformBuffer,
    Sampler,
    AccelerationStructure,
};

/** The execution stage reflected from the selected Slang entry point. */
enum class ShaderStage {
    /** The load path may link a module without its entry points; stage is then advisory. */
    Unknown,
    Vertex,
    Fragment,
    Compute,
    RayGeneration,
};

/** One selected Slang entry point and the stage Vulkan will create it for. */
struct ShaderEntryPoint {
    std::string name;
    ShaderStage stage {ShaderStage::Unknown};
};

struct ShaderBindingDescription {
    std::string name;
    uint32_t set {0};
    uint32_t binding {0};
    uint32_t count {1};
    ShaderResourceKind kind {ShaderResourceKind::StorageBuffer};
};

struct ShaderReflection {
    std::vector<ShaderBindingDescription> bindings;
    uint32_t pushConstantSize {0};
    ShaderStage stage {ShaderStage::Unknown};
};

/** An opaque descriptor location resolved from a Slang resource name. */
struct ComputeResourceSlot {
    uint32_t value {0};
};

/** One scalar or array resource bound to a reflected compute-shader slot. */
struct ComputeBinding {
    enum class Type { Image, Buffer };

    ComputeResourceSlot slot;
    Type type {Type::Image};
    ImageView image;
    BufferView buffer;
    const ImageView *imageArray {nullptr};
    const BufferView *bufferArray {nullptr};
    uint32_t count {0};

    ComputeBinding(ComputeResourceSlot slot, ImageView image) :
        slot(slot), image(image), count(1) {}
    ComputeBinding(ComputeResourceSlot slot, const ImageView *images, uint32_t count) :
        slot(slot), imageArray(images), count(count) {}
    ComputeBinding(ComputeResourceSlot slot, BufferView buffer) :
        slot(slot), type(Type::Buffer), buffer(buffer), count(1) {}
    ComputeBinding(ComputeResourceSlot slot, const BufferView *buffers, uint32_t count) :
        slot(slot), type(Type::Buffer), bufferArray(buffers), count(count) {}
};

/** Explicit bindings kept for one pass or frame; dispatches may override them. */
struct ComputeBindingSet {
    const ComputeBinding *bindings {nullptr};
    uint32_t count {0};
};

struct ComputePipelineDesc {
    std::string shader;
    std::string entry {"main"};
    uint32_t descriptorSetCopies {1};
};

class IComputePipeline {
public:
    virtual ~IComputePipeline() = default;

    /** Resolve these Slang resource names once while the pipeline is created. */
    virtual std::vector<ComputeResourceSlot> resolveBindings(
        std::initializer_list<const char *> names) const = 0;
    virtual const std::vector<ShaderBindingDescription> &bindings() const = 0;
};

} // namespace reone::graphics
