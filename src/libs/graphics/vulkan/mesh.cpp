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

#include "reone/graphics/vulkan/mesh.h"

#include "reone/graphics/vulkan/device.h"

#include "reone/system/logutil.h"

namespace reone {

namespace graphics {

std::vector<VkVertexInputBindingDescription> VulkanMesh::bindingDescriptions(
    const Mesh::VertexLayout &layout) {
    VkVertexInputBindingDescription mesh {};
    mesh.binding = 0;
    mesh.stride = static_cast<uint32_t>(layout.stride);
    mesh.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // Stride 0: every vertex reads the same element, which is all zeros.
    VkVertexInputBindingDescription zeros {};
    zeros.binding = kZeroBinding;
    zeros.stride = 0;
    zeros.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    return {mesh, zeros};
}

std::vector<VkVertexInputAttributeDescription> VulkanMesh::attributeDescriptions(
    const Mesh::VertexLayout &layout) {
    std::vector<VkVertexInputAttributeDescription> attributes;

    auto add = [&attributes](uint32_t location, VkFormat format, int offset) {
        VkVertexInputAttributeDescription attribute {};
        attribute.location = location;
        attribute.format = format;
        if (offset == -1) {
            // Not in this mesh: read zeros rather than omit the location, which
            // would be a pipeline creation error.
            attribute.binding = kZeroBinding;
            attribute.offset = 0;
        } else {
            attribute.binding = 0;
            attribute.offset = static_cast<uint32_t>(offset);
        }
        attributes.push_back(attribute);
    };

    // Locations match the [[vk::location(n)]] in the Slang shaders, which match
    // the attribute indices the GL path binds. Changing one means changing all
    // three.
    add(0, VK_FORMAT_R32G32B32_SFLOAT, layout.offPosition);
    add(1, VK_FORMAT_R32G32B32_SFLOAT, layout.offNormals);
    add(2, VK_FORMAT_R32G32_SFLOAT, layout.offUV1);
    add(3, VK_FORMAT_R32G32_SFLOAT, layout.offUV2);

    // One offset covers three consecutive vec3s, in this order. The naming is
    // confusing and matches the GL path: bitangent first, then tangent, then
    // the tangent-space normal.
    constexpr int kVec3 = 3 * sizeof(float);
    bool hasTanSpace = layout.offTanSpace != -1;
    add(4, VK_FORMAT_R32G32B32_SFLOAT, hasTanSpace ? layout.offTanSpace : -1);
    add(5, VK_FORMAT_R32G32B32_SFLOAT, hasTanSpace ? layout.offTanSpace + kVec3 : -1);
    add(6, VK_FORMAT_R32G32B32_SFLOAT, hasTanSpace ? layout.offTanSpace + 2 * kVec3 : -1);

    // Bone indices are floats in the buffer, not integers, as in the GL path.
    add(7, VK_FORMAT_R32G32B32A32_SFLOAT, layout.offBoneIndices);
    add(8, VK_FORMAT_R32G32B32A32_SFLOAT, layout.offBoneWeights);
    add(9, VK_FORMAT_R32_SFLOAT, layout.offMaterial);

    return attributes;
}

void VulkanMesh::init(const Mesh &mesh) {
    const auto &vertexData = mesh.vertexData();
    if (vertexData.empty()) {
        throw std::invalid_argument("Vulkan: mesh has no vertex data");
    }
    const auto &layout = mesh.vertexLayout();
    if (layout.stride <= 0 || layout.offPosition < 0) {
        throw std::invalid_argument("Vulkan: mesh has no position data");
    }
    VkBufferUsageFlags vertexUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBufferUsageFlags indexUsage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (_device.rayQueryAvailable()) {
        // Source data for the merge lives in VulkanResources' shared buffers.
        // These per-mesh buffers are now raster-only.
    }
    _vertexBuffer.initDeviceLocal(vertexData.size() * sizeof(float),
                                  vertexUsage,
                                  vertexData.data());

    // Faces carry uint16 indices, which is what the index buffer stores.
    std::vector<uint16_t> indices;
    indices.reserve(mesh.faces().size() * 3);
    for (const auto &face : mesh.faces()) {
        indices.push_back(face.vertices[0]);
        indices.push_back(face.vertices[1]);
        indices.push_back(face.vertices[2]);
    }
    if (indices.empty()) {
        throw std::invalid_argument("Vulkan: mesh has no faces");
    }
    _indexBuffer.initDeviceLocal(indices.size() * sizeof(uint16_t),
                                 indexUsage,
                                 indices.data());
    _indexCount = static_cast<uint32_t>(indices.size());

}

void VulkanMesh::deinit() {
    _vertexBuffer.deinit();
    _indexBuffer.deinit();
    _indexCount = 0;
}

void VulkanMesh::draw(VkCommandBuffer cmd, VkBuffer zeros, int instances) const {
    VkBuffer buffers[] {_vertexBuffer.handle(), zeros};
    VkDeviceSize offsets[] {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, _indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, _indexCount, static_cast<uint32_t>(instances), 0, 0, 0);
}

} // namespace graphics

} // namespace reone
