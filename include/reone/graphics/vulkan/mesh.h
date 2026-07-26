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

#include <volk.h>

#include "reone/graphics/mesh.h"

#include "buffer.h"

namespace reone {

namespace graphics {

class VulkanDevice;

/**
 * A Mesh uploaded to device-local memory: its interleaved vertex data and an
 * index buffer built from its faces.
 *
 * The vertex layout is not reinterpreted. Mesh already describes itself as a
 * stride and a set of offsets, and the attribute locations are the ones the
 * Slang shaders declare with [[vk::location(n)]], which are in turn the ones
 * the GL path binds. All three have to agree, so there is one description of
 * them, here.
 */
class VulkanMesh : boost::noncopyable {
public:
    VulkanMesh(VulkanDevice &device) :
        _device(device),
        _vertexBuffer(device),
        _indexBuffer(device) {
    }

    void init(const Mesh &mesh);
    void deinit();

    /** Bind the buffers and issue an indexed draw. */
    void draw(VkCommandBuffer cmd, int instances = 1) const;

    /**
     * Vertex input state for a pipeline drawing this layout. The returned
     * attributes reference @p layout only through values, so the caller may
     * discard it.
     */
    static VkVertexInputBindingDescription bindingDescription(const Mesh::VertexLayout &layout);
    static std::vector<VkVertexInputAttributeDescription> attributeDescriptions(
        const Mesh::VertexLayout &layout);

    uint32_t indexCount() const { return _indexCount; }

private:
    VulkanDevice &_device;

    VulkanBuffer _vertexBuffer;
    VulkanBuffer _indexBuffer;
    uint32_t _indexCount {0};
};

} // namespace graphics

} // namespace reone
