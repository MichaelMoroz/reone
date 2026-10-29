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
    /**
     * Primary-vertex direct light, for the second denoiser.
     *
     * Its own channel and its own denoiser instance, because it is a different
     * signal: converged almost everywhere and noisy only inside a penumbra,
     * where the indirect channel is noisy throughout. Summed into the diffuse
     * channel it gets a kernel chosen for the bounce noise - one variance per
     * pixel, dominated by the wrong term - and the shadow detail goes with it.
     * Null leaves the second denoiser unrecorded.
     */
    IImage *directRadianceHitDist {nullptr};
};

/**
 * Which NRD denoiser the tracer feeds. The two want incompatible inputs - see
 * TracingDenoiserTuning - so the choice reaches the trace kernel as well, and
 * changing it rebuilds the instance rather than being live.
 */
/**
 * Live tuning, in NRD's own units where they are shared and its own names where
 * they are not. Several fields apply to one denoiser only, marked below; the
 * other reads its own set and ignores the rest.
 *
 * Accumulation is expressed in seconds rather than frames. NRD measures history
 * in frames for simplicity but says not to configure it that way: "recalculate
 * the number of accumulated frames from the accumulation time... it allows to
 * minimize lags if FPS is low and maximize IQ if FPS is high", its own defaults
 * being quoted for 60 FPS. A fixed frame count is why the denoiser got worse
 * the faster the frame rate went - at 200 FPS a thirty-frame history is 150 ms
 * of light, and the spatial filter widens to cover what the history no longer
 * carries.
 */
struct TracingDenoiserTuning {
    /** Seconds of history. NRD's constant for both denoisers is 0.5. */
    float accumulationTime {0.5f};
    /** Seconds of responsive history, clamped below the above. */
    float fastAccumulationTime {0.1f};
    int historyFixFrames {3};
    float diffusePrepassBlurRadius {30.0f};
    float specularPrepassBlurRadius {50.0f};
    float lobeAngleFraction {0.15f};
    float roughnessFraction {0.15f};
    float disocclusionThreshold {0.01f};
    bool antiFirefly {true};
    int atrousIterations {5};
    float diffusePhiLuminance {2.0f};
    float specularPhiLuminance {1.0f};
    float depthThreshold {0.003f};
    float specularLobeAngleSlack {0.15f};

    /**
     * The direct-light denoiser's own dials, separate on purpose.
     *
     * Shorter history than the bounce channel because a shadow edge moves with
     * whatever casts it, where indirect light changes slowly; and fewer A-trous
     * passes with a tighter luminance phi because the signal arrives converged
     * outside the penumbra, so every extra iteration is width spent on detail
     * rather than on noise.
     */
    float directAccumulationTime {0.15f};
    int directAtrousIterations {3};
    float directPhiLuminance {1.0f};
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
    /** Null unless the instance carries the direct-light denoiser. */
    virtual IImage *denoisedDirect() = 0;
};

} // namespace reone::graphics
