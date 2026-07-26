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

    /**
     * Bind the buffers and issue an indexed draw.
     *
     * @param zeros the shared stride-0 buffer bound at kZeroBinding
     */
    void draw(VkCommandBuffer cmd, VkBuffer zeros, int instances = 1) const;

    /**
     * Vertex input state for a pipeline drawing this layout. The returned
     * descriptions reference @p layout only through values, so the caller may
     * discard it.
     *
     * Two bindings. Binding 0 is the mesh's own interleaved data. Binding 1 has
     * stride 0 and holds zeros, and every attribute the mesh does not provide
     * points at it.
     *
     * OpenGL gives an attribute the shader declares but the buffer omits a
     * default value; Vulkan makes the missing attribute an error. The game's
     * meshes genuinely vary - plenty have no tangent frame or bone weights, and
     * are drawn by a shader that declares both - so the difference has to be
     * made up somewhere. A stride-0 binding does it without an extension and
     * without aliasing one attribute onto another's bytes, which would feed
     * positions to a shader asking for normals.
     */
    static std::vector<VkVertexInputBindingDescription> bindingDescriptions(
        const Mesh::VertexLayout &layout);
    static std::vector<VkVertexInputAttributeDescription> attributeDescriptions(
        const Mesh::VertexLayout &layout);

    /** Binding index of the zero-filled stride-0 buffer. */
    static constexpr uint32_t kZeroBinding = 1;

    uint32_t indexCount() const { return _indexCount; }

private:
    VulkanDevice &_device;

    VulkanBuffer _vertexBuffer;
    VulkanBuffer _indexBuffer;
    uint32_t _indexCount {0};
};

} // namespace graphics

} // namespace reone
