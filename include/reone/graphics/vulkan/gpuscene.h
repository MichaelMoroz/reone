/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <volk.h>

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include "reone/graphics/gpuscene.h"

namespace reone::graphics {

class VulkanBuffer;
class VulkanPipeline;
class VulkanRenderer;

/** Native storage and compute publication for a Vulkan-free scene upload. */
class VulkanGpuScene : boost::noncopyable {
public:
    struct BufferView {
        const VulkanBuffer *buffer {nullptr};
        VkDeviceSize offset {0};
        VkDeviceSize size {0};
    };
    struct PrimitiveId {
        uint64_t sceneScope {0};
        uint32_t objectIndex {0};
        uint32_t objectGeneration {0};
        uint32_t localPrimitive {0};
    };
    struct PrimitiveIdRange {
        uint32_t firstTriangle {0};
        uint32_t triangleCount {0};
        PrimitiveId first;
    };
    struct PrimitiveIdView {
        const PrimitiveIdRange *ranges {nullptr};
        uint32_t rangeCount {0};
        PrimitiveId operator[](uint32_t index) const;
    };
    struct Region {
        GpuSceneResidencyClass residency {GpuSceneResidencyClass::Dynamic};
        uint64_t revision {0};
        uint32_t firstVertex {0};
        uint32_t vertexCount {0};
        uint32_t firstTriangle {0};
        uint32_t triangleCount {0};
    };
    struct View {
        uint64_t sceneScope {0};
        uint64_t revision {0};
        BufferView vertices;
        BufferView indices;
        BufferView materialIds;
        BufferView materials;
        uint32_t objectCount {0};
        uint32_t opaqueObjectCount {0};
        uint32_t vertexCount {0};
        uint32_t opaqueTriangleCount {0};
        uint32_t triangleCount {0};
        PrimitiveIdView primitiveIds;
        std::vector<Region> regions;
    };

    VulkanGpuScene();
    ~VulkanGpuScene();

    void init(VulkanRenderer &renderer);
    void deinit();
    View update(VkCommandBuffer cmd, GpuSceneUpload &upload);

private:
    struct Frame;
    struct SourceGeometry {
        uint32_t vertexOffset {0};
        uint32_t indexOffset {0};
        uint32_t vertexDataCount {0};
        uint32_t indexCount {0};
    };

    VulkanRenderer *_renderer {nullptr};
    std::unique_ptr<VulkanPipeline> _mergePipeline;
    std::array<std::unique_ptr<Frame>, 2> _frames;
    std::unordered_map<const Mesh *, SourceGeometry> _sourceGeometry;
    std::vector<float> _sourceVertexData;
    std::vector<uint32_t> _sourceIndexData;
    std::unique_ptr<VulkanBuffer> _sourceVertices;
    std::unique_ptr<VulkanBuffer> _sourceIndices;
    std::unique_ptr<VulkanBuffer> _grassFaces;
    std::vector<std::unique_ptr<VulkanBuffer>> _retiredSourceBuffers;
    uint32_t _sourceVertexCapacity {0};
    uint32_t _sourceIndexCapacity {0};
    uint64_t _grassFaceGeneration {0};
    uint64_t _sourceResourceGeneration {0};
    uint64_t _sceneScope {1};
    uint64_t _revision {0};
    bool _inited {false};

    void ensureMergeBuffers(Frame &, uint32_t, uint32_t, uint32_t, uint32_t,
                            uint32_t, uint32_t, uint32_t);
    void clearSourceGeometry();
    const SourceGeometry &appendSourceGeometry(const Mesh &mesh);
};

} // namespace reone::graphics
