/*
 * Copyright (c) 2026 The reone project contributors
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
#include <string>

#include "buffer.h"
#include "computepipeline.h"
#include "rhi.h"

namespace reone::graphics {

class ICommandBuffer;
class IImage;
class ITracingStructure;

/** One resource selected by its reflected ray-tracing shader name. */
struct TracingBinding {
    const char *name {nullptr};
    const IImage *image {nullptr};
    BufferView buffer;
    ITracingStructure *structure {nullptr};
    const IImage *const *images {nullptr};
    uint32_t imageCount {0};
    uint32_t arrayIndex {0};
    bool arrayElement {false};
    ImageView imageView;
    bool hasImageView {false};

    TracingBinding(const char *name, const IImage &image) : name(name), image(&image) {}
    TracingBinding(const char *name, BufferView buffer) : name(name), buffer(buffer) {}
    TracingBinding(const char *name, ITracingStructure &structure) : name(name), structure(&structure) {}
    TracingBinding(const char *name, const IImage *const *images, uint32_t imageCount) :
        name(name), images(images), imageCount(imageCount) {}
};

struct TracingBindingSet {
    const TracingBinding *bindings {nullptr};
    uint32_t count {0};
};

struct TracingPipelineDesc {
    std::string shader;
    ShaderReflection reflection;
    uint32_t pushConstantSize {0};
    std::string label;
};

/** A reflected ray-generation pipeline and its frame-local descriptor sets. */
class ITracingPipeline {
public:
    virtual ~ITracingPipeline() = default;

    virtual Pipeline pipeline() const = 0;
    virtual PipelineLayout pipelineLayout() const = 0;
    virtual DescriptorSet descriptorSet(uint32_t set, uint32_t frameIndex) const = 0;
    virtual uint32_t bindlessTextureCapacity() const = 0;
    virtual void updateBindings(uint32_t set, uint32_t frameIndex,
                                const TracingBindingSet &bindings) = 0;
};

/** Inputs and live tuning for the path tracer's temporal denoiser. */
struct TracingDenoiserInputs {
    IImage *motion {nullptr};
    IImage *normalRoughness {nullptr};
    IImage *viewZ {nullptr};
    IImage *diffRadianceHitDist {nullptr};
    IImage *specRadianceHitDist {nullptr};
};

struct TracingDenoiserTuning {
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

class ITracingDenoiser {
public:
    virtual ~ITracingDenoiser() = default;

    virtual void denoise(ICommandBuffer &commandBuffer, int frameIndex,
                         const TracingDenoiserInputs &inputs,
                         const TracingDenoiserTuning &tuning,
                         const glm::mat4 &view, const glm::mat4 &projection,
                         const glm::vec2 &jitter, uint32_t frameNumber,
                         bool restartHistory) = 0;
    virtual IImage &denoisedDiffuse() = 0;
    virtual IImage &denoisedSpecular() = 0;
};

} // namespace reone::graphics
