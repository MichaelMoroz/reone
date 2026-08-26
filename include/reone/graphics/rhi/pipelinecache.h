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

#include <array>
#include <string>
#include <memory>
#include <optional>
#include <vector>

#include "rhi.h"
#include "../mesh.h"
#include "../types.h"
#include "../rendering/gpuscene.h"
#include "../rendering/sky.h"

namespace reone {

namespace graphics {

/**
 * Shared push-constant range exposed by every cached graphics/compute layout.
 * ResolvePushConstants is the largest caller at eight 32-bit words; smaller
 * passes may update only the prefix they use.
 */
constexpr uint32_t kCachedPipelinePushConstantSize = 10 * sizeof(uint32_t);

class ICommandBuffer;
class IImage;
class ITracingStructure;
class Texture;
struct GraphicsOptions;

/** Results read from the previous frame's trace counters. */
struct TracingStats {
    uint32_t secondaryRays {0};
    uint32_t secondaryMisses {0};
    uint32_t survivingLights {0};
    uint32_t primaryHits {0};
    uint32_t shadowRays {0};
    uint32_t bindlessTextures {0};
};

/**
 * The rasterized primary, handed to the tracer instead of a camera ray.
 *
 * Every mode runs the geometry pass before it shades anything, so by the time
 * the tracer starts these attachments already say which surface each pixel
 * shows. The kernel reads its primary out of them and traces only outwards; the
 * images must therefore be published as sampled before the trace is recorded,
 * not after it as they were when they were validation targets only.
 */
struct GBufferBinding {
    IImage *diffuse {nullptr};
    IImage *eyeNormal {nullptr};
    IImage *lightmap {nullptr};
    IImage *selfIllum {nullptr};
    IImage *motion {nullptr};
    IImage *depth {nullptr};
    IImage *triangleId {nullptr};
};

/**
 * The tracer's storage-output channels, owned by ScenePipeline and written by
 * the trace kernel's set 2.
 *
 * Ownership sits with ScenePipeline because the composite that assembles them
 * into the final image is a ScenePipeline pass, and a raster mode will one day
 * shade into the same channels without the tracer running at all. The tracer
 * receives the images for the frame and binds them by name into its own set 2,
 * exactly as it binds the G-buffer block. Index order is the trace kernel's aux
 * order (tracing/outputs.slang); the count is asserted where they are named.
 */
constexpr int kNumTracingChannels = 15;
struct ChannelBinding {
    std::array<IImage *, kNumTracingChannels> images {};
};

/**
 * What the trace pass hands back for ScenePipeline's composite.
 *
 * The channel images are ScenePipeline's own; these are the pieces only the
 * tracer can produce - NRD's denoised radiance pair and whichever image settled
 * the direct-light channel - plus the two push-constant values the composite
 * needs. runComposite folds the tracer-only knowledge of whether a denoiser
 * even exists together with the denoise/debug-view gate: false means the trace
 * kernel already wrote the final image itself and the composite must stand
 * aside.
 */
struct TracingPipelineOutput {
    bool runComposite {false};
    ImageView denoisedDiffuse;
    ImageView denoisedSpecular;
    ImageView directDiffuse;
    float denoisedJitter[2] {0.0f, 0.0f};
    uint32_t directDenoised {0};
    uint32_t debugView {0};
};

/** Everything the tracing implementation needs to turn one merged scene into pixels. */
struct TracingPipelineInput {
    ICommandBuffer &commandBuffer;
    uint32_t globalsOffset {0};
    IImage &output;
    const glm::mat4 &view;
    const glm::mat4 &projection;
    const glm::vec4 &jitter;
    const GpuScene::View &scene;
    ITracingStructure &structure;
    int frameIndex {0};
    uint32_t frameNumber {0};
    SkyBinding sky;
    GBufferBinding gbuffer;
    /** ScenePipeline's channel images for this frame; the kernel writes them. */
    ChannelBinding channels;
    /** Filled by the trace pass; read by ScenePipeline::compositePass. */
    TracingPipelineOutput *composite {nullptr};
};

struct TracingChannel {
    const char *name {nullptr};
    const char *dumpName {nullptr};
    IImage *image {nullptr};
};

/**
 * Traces an admitted merged scene, including its native descriptor, denoising
 * and upscaling work. The caller supplies scene meaning; the backend owns how
 * that work is expressed to its API.
 */
/** Pipeline state selected by the 2D and image-based-lighting clients. */
struct PipelineKey {
    std::string module;
    std::string vertexEntry;
    std::string fragmentEntry;
    /**
     * Set instead of the two above to build a compute pipeline over the same
     * descriptor set layouts every graphics pipeline here uses.
     *
     * The cache exists so that a pass can name a kernel and get one back with
     * the frame uniforms, the texture table, the merged material set and the
     * resolve set already in its layout. A dispatch whose resources are its own
     * wants IRenderer::makeComputePipeline instead, which reflects them.
     */
    std::string computeEntry;
    std::vector<Format> colorFormats;
    Format depthFormat {Format::D32Sfloat};
    uint32_t viewMask {0};
    BlendMode blend {BlendMode::None};
    bool depthTest {false};
    bool depthWrite {false};
    bool depthBias {false};
    float depthBiasConstantFactor {0.0f};
    float depthBiasSlopeFactor {0.0f};
    FaceCullMode cull {FaceCullMode::None};
    /** Empty when the vertex shader synthesises geometry from SV_VertexID. */
    std::optional<Mesh::VertexLayout> vertexLayout;
};

struct PipelineBinding {
    Pipeline pipeline;
    PipelineLayout layout;
};

/** Pipeline-cache operation used by the 2D and image-based-lighting clients. */
class IPipelineCache {
public:
    virtual ~IPipelineCache() = default;

    virtual PipelineBinding get(const PipelineKey &key) = 0;

};

} // namespace graphics

} // namespace reone
