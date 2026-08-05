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

#include "rhi.h"

namespace reone {

namespace graphics {

class IImage;

/** The layouts the image-based-lighting client transitions between. */
enum class ImageLayout {
    ShaderRead,
    ColorAttachment,
};

/** Commands recorded by the 2D and image-based-lighting clients. */
class ICommandBuffer {
public:
    virtual ~ICommandBuffer() = default;

    virtual void transitionImage(IImage &image, ImageLayout to) = 0;
    virtual void bindPipeline(Pipeline pipeline) = 0;
    virtual void bindDescriptorSet(PipelineLayout layout, uint32_t index,
                                   DescriptorSet set,
                                   const uint32_t *dynamicOffsets,
                                   uint32_t dynamicOffsetCount) = 0;
    virtual void draw(uint32_t vertexCount, uint32_t instanceCount) = 0;
    virtual void setScissor(glm::ivec2 offset, glm::uvec2 extent) = 0;
};

} // namespace graphics

} // namespace reone
