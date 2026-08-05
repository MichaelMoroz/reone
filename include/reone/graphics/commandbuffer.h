/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <vector>

#include "buffer.h"
#include "rhi.h"
#include "tracingstructure.h"

namespace reone {

namespace graphics {

class IImage;

/** The layouts the image-based-lighting client transitions between. */
enum class ImageLayout {
    ShaderRead,
    ColorAttachment,
    DepthAttachment,
    DepthRead,
    General,
};

enum class AttachmentLoad { DontCare, Load, Clear };
enum class AttachmentStore { DontCare, Store };

/** A pass clear is expressed in the value domain of the attachment. */
struct ClearValue {
    glm::vec4 color {0.0f};
    uint32_t uintValue {0};
    float depth {1.0f};
    bool integer {false};
    bool depthOnly {false};
};

/** One image view read by a rendering pass as an attachment. */
struct RenderAttachment {
    ImageView view;
    ImageLayout layout {ImageLayout::ColorAttachment};
    AttachmentLoad load {AttachmentLoad::DontCare};
    AttachmentStore store {AttachmentStore::DontCare};
    ClearValue clear;
};

/** Commands recorded by the 2D and image-based-lighting clients. */
class ICommandBuffer {
public:
    virtual ~ICommandBuffer() = default;

    virtual void transitionImage(IImage &image, ImageLayout to) = 0;
    virtual void bindPipeline(Pipeline pipeline) = 0;
    virtual void bindComputePipeline(Pipeline pipeline) = 0;
    virtual void bindRayTracingPipeline(Pipeline pipeline) = 0;
    virtual void bindDescriptorSet(PipelineLayout layout, uint32_t index,
                                   DescriptorSet set,
                                   const uint32_t *dynamicOffsets,
                                   uint32_t dynamicOffsetCount) = 0;
    virtual void bindComputeDescriptorSet(PipelineLayout layout, uint32_t index,
                                          DescriptorSet set,
                                          const uint32_t *dynamicOffsets,
                                          uint32_t dynamicOffsetCount) = 0;
    virtual void bindRayTracingDescriptorSet(PipelineLayout layout, uint32_t index,
                                             DescriptorSet set,
                                             const uint32_t *dynamicOffsets,
                                             uint32_t dynamicOffsetCount) = 0;
    virtual void draw(uint32_t vertexCount, uint32_t instanceCount) = 0;
    virtual void setScissor(glm::ivec2 offset, glm::uvec2 extent) = 0;
    virtual void beginRendering(glm::ivec2 extent,
                                const std::vector<RenderAttachment> &colors,
                                const RenderAttachment *depth,
                                uint32_t viewMask,
                                bool invertedViewport) = 0;
    virtual void endRendering() = 0;
    virtual void bindIndexBuffer(const IBuffer &buffer, uint64_t offset) = 0;
    virtual void drawIndexed(uint32_t indexCount, uint32_t firstIndex) = 0;
    virtual void pushFragmentConstants(PipelineLayout layout, const void *data,
                                       uint32_t size) = 0;
    virtual void pushComputeConstants(PipelineLayout layout, const void *data,
                                      uint32_t size) = 0;
    virtual void pushRayTracingConstants(PipelineLayout layout, const void *data,
                                         uint32_t size) = 0;
    virtual void dispatch(glm::uvec3 groups) = 0;
    /** Clear a color target before a pass with no geometry to render. */
    virtual void clearColor(IImage &image, glm::vec4 color) = 0;
    /** Make freshly uploaded scene sources readable by the merge compute pass. */
    virtual void makeGpuSceneSourcesAvailable(const IBuffer &vertices,
                                              const IBuffer &indices) = 0;
    /** Publish merge-compute output to every scene geometry consumer. */
    virtual void publishMergedScene() = 0;
    /** Build this frame's scene-wide tracing structure over merged geometry. */
    virtual void buildSceneTracingStructure(ITracingStructure &structure,
                                            const SceneTracingGeometry &geometry) = 0;
    /** Trace a ray grid against this frame's scene-wide tracing structure. */
    virtual void traceRays(Pipeline pipeline, ITracingStructure &structure,
                           glm::uvec2 extent) = 0;
    /** Make trace storage writes available to denoising reads. */
    virtual void publishTraceOutputForDenoising() = 0;
    /** Make the composite's storage writes available to the upscaler. */
    virtual void publishCompositeForUpscaling() = 0;
    /** Return the upscaler's sampled inputs to storage-write use next frame. */
    virtual void restoreUpscalerInputsForNextFrame(IImage &color, IImage &depth,
                                                   IImage &motion) = 0;
    /** Make the upscaler's HDR output available to the tone-map pass. */
    virtual void publishUpscaledFrameForTonemapping() = 0;
};

} // namespace graphics

} // namespace reone
