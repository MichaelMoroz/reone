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

namespace reone {

namespace graphics {

VkVertexInputBindingDescription VulkanMesh::bindingDescription(const Mesh::VertexLayout &layout) {
    VkVertexInputBindingDescription binding {};
    binding.binding = 0;
    binding.stride = static_cast<uint32_t>(layout.stride);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return binding;
}

std::vector<VkVertexInputAttributeDescription> VulkanMesh::attributeDescriptions(
    const Mesh::VertexLayout &layout) {
    std::vector<VkVertexInputAttributeDescription> attributes;

    auto add = [&attributes](uint32_t location, VkFormat format, int offset) {
        if (offset == -1) {
            return;
        }
        VkVertexInputAttributeDescription attribute {};
        attribute.location = location;
        attribute.binding = 0;
        attribute.format = format;
        attribute.offset = static_cast<uint32_t>(offset);
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
    if (layout.offTanSpace != -1) {
        constexpr int kVec3 = 3 * sizeof(float);
        add(4, VK_FORMAT_R32G32B32_SFLOAT, layout.offTanSpace);
        add(5, VK_FORMAT_R32G32B32_SFLOAT, layout.offTanSpace + kVec3);
        add(6, VK_FORMAT_R32G32B32_SFLOAT, layout.offTanSpace + 2 * kVec3);
    }

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
    _vertexBuffer.initDeviceLocal(vertexData.size() * sizeof(float),
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
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
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 indices.data());
    _indexCount = static_cast<uint32_t>(indices.size());
}

void VulkanMesh::deinit() {
    _vertexBuffer.deinit();
    _indexBuffer.deinit();
    _indexCount = 0;
}

void VulkanMesh::draw(VkCommandBuffer cmd, int instances) const {
    VkBuffer buffers[] {_vertexBuffer.handle()};
    VkDeviceSize offsets[] {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
    vkCmdBindIndexBuffer(cmd, _indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, _indexCount, static_cast<uint32_t>(instances), 0, 0, 0);
}

} // namespace graphics

} // namespace reone
