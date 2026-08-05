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

#include <vector>

#include "buffer.h"
#include "computepipeline.h"
#include "rhi.h"
#include "tracingstructure.h"

namespace reone {

namespace graphics {

class IImage;
class ICommandBuffer;

/** A labelled command-buffer region, closed even when its caller returns early. */
class CommandBufferDebugScope {
public:
    CommandBufferDebugScope(ICommandBuffer &commandBuffer, const char *name,
                            const glm::vec3 &color = {0.4f, 0.6f, 0.9f});
    ~CommandBufferDebugScope();

    CommandBufferDebugScope(const CommandBufferDebugScope &) = delete;
    CommandBufferDebugScope &operator=(const CommandBufferDebugScope &) = delete;

private:
    ICommandBuffer &_commandBuffer;
};

/** The layouts the image-based-lighting client transitions between. */
enum class ImageLayout {
    ShaderRead,
    ColorAttachment,
    DepthAttachment,
    DepthRead,
    General,
};

/** The way a buffer participates in a dependency. */
enum class BufferUse {
    TransferWrite,
    ComputeRead,
    ComputeWrite,
    AccelerationStructureBuildRead,
    ShaderRead,
    IndexRead,
};

/** The way an image participates in a dependency. */
enum class ImageUse {
    RayTracingStore,
    ComputeRead,
    ComputeSample,
    ComputeStore,
    ComputeStorageRead,
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
    /** Move several images to one layout in a single dependency. */
    virtual void transitionImages(const std::vector<IImage *> &images, ImageLayout to) = 0;
    virtual void beginDebugScope(const char *name, const glm::vec3 &color) = 0;
    virtual void endDebugScope() = 0;
    virtual void bindPipeline(Pipeline pipeline) = 0;
    virtual void bindRayTracingPipeline(Pipeline pipeline) = 0;
    virtual void bindDescriptorSet(PipelineLayout layout, uint32_t index,
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
    virtual void pushRayTracingConstants(PipelineLayout layout, const void *data,
                                         uint32_t size) = 0;
    /** Bind reflected compute resources and record one compute dispatch. */
    virtual void dispatch(IComputePipeline &shader, glm::uvec3 groups,
                          const ComputeBindingSet &bindings,
                          const ComputeBindingSet *overrides = nullptr,
                          const void *pushConstants = nullptr,
                          uint32_t pushConstantSize = 0) = 0;
    /** Clear a color target before a pass with no geometry to render. */
    virtual void clearColor(IImage &image, glm::vec4 color) = 0;
    virtual void bufferBarrier(IBuffer &buffer, BufferUse from, BufferUse to) = 0;
    virtual void imageBarrier(IImage &image, ImageUse from, ImageUse to) = 0;
    /** Build this frame's scene-wide tracing structure over merged geometry. */
    virtual void buildSceneTracingStructure(ITracingStructure &structure,
                                            const SceneTracingGeometry &geometry) = 0;
    /** Trace a ray grid against this frame's scene-wide tracing structure. */
    virtual void traceRays(Pipeline pipeline, ITracingStructure &structure,
                           glm::uvec2 extent) = 0;
};

} // namespace graphics

} // namespace reone
