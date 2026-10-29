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

#include "reone/graphics/rendering/gbuffer.h"

#include "reone/graphics/rhi/commandbuffer.h"
#include "reone/graphics/rhi/resources.h"

namespace reone::graphics {

size_t GBuffer::attachmentIndex(GBufferAttachment attachment) {
    switch (attachment) {
    case GBufferAttachment::Diffuse: return 0;
    case GBufferAttachment::EyeNormal: return 1;
    case GBufferAttachment::Lightmap: return 2;
    case GBufferAttachment::SelfIllum: return 3;
    case GBufferAttachment::Motion: return 4;
    case GBufferAttachment::TriangleId: return 5;
    }
    throw std::invalid_argument("Unknown G-buffer attachment");
}

void GBuffer::init(glm::ivec2 extent) {
    _extent = extent;
    auto formats = colorFormats();
    auto &resources = _renderer.resources();
    for (size_t i = 0; i < _color.size(); ++i) {
        _color[i] = resources.makeImage();
        _color[i]->initColorAttachment(extent, formats[i]);
    }
    _depth = resources.makeImage();
    _depth->initDepthAttachment(extent, depthFormat());

    // Images begin undefined and dynamic rendering does not transition them.
    // Keep the colour attachments in one dependency; later passes preserve
    // that batching through ICommandBuffer::transitionImages as well.
    _renderer.immediateSubmit([this](ICommandBuffer &commandBuffer) {
        commandBuffer.transitionImages(colorImages(), ImageLayout::ColorAttachment);
        commandBuffer.transitionImage(*_depth, ImageLayout::DepthAttachment);
    });
}

void GBuffer::deinit() {
    for (auto &image : _color) {
        image.reset();
    }
    _depth.reset();
}

void GBuffer::setSamplers(Sampler colorSampler, Sampler depthSampler,
                          Sampler triangleIdSampler) {
    for (auto &image : _color) {
        image->setSampler(colorSampler);
    }
    color(GBufferAttachment::TriangleId).setSampler(triangleIdSampler);
    _depth->setSampler(depthSampler);
}

IImage &GBuffer::color(GBufferAttachment attachment) {
    return *_color[attachmentIndex(attachment)];
}

std::vector<IImage *> GBuffer::colorImages() {
    std::vector<IImage *> images;
    images.reserve(_color.size());
    for (auto &image : _color) {
        images.push_back(image.get());
    }
    return images;
}

std::vector<Format> GBuffer::colorFormats() const {
    return {Format::R8G8B8A8Unorm, Format::R8G8B8A8Unorm,
            Format::R8G8B8A8Unorm, Format::R8G8B8A8Unorm,
            Format::R16G16Sfloat, Format::R32Uint};
}

} // namespace reone::graphics
