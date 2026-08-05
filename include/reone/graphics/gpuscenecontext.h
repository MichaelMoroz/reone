/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <memory>
#include "rhi/buffer.h"
#include "rhi/commandbuffer.h"

namespace reone::graphics {

class Mesh;

struct GpuSceneMerge {
    const BufferView *buffers {nullptr};
    uint32_t bufferCount {0};
    uint32_t frameIndex {0};
    uint32_t objectCount {0};
    uint32_t opaqueObjectCount {0};
    uint32_t vertexCount {0};
    uint32_t triangleCount {0};
    uint32_t opaqueTriangleCount {0};
    glm::vec4 cameraPosition {0.0f};
};

class IGpuSceneMergePipeline {
public:
    virtual ~IGpuSceneMergePipeline() = default;

    virtual void merge(ICommandBuffer &commandBuffer, const GpuSceneMerge &merge) = 0;
};

/** Backend services used to publish one GpuScene upload. */
class IGpuSceneContext {
public:
    virtual ~IGpuSceneContext() = default;

    virtual std::unique_ptr<IBuffer> makeBuffer() = 0;
    virtual std::unique_ptr<IGpuSceneMergePipeline> makeGpuSceneMergePipeline() = 0;
    virtual void prepareMesh(const Mesh &mesh) = 0;
    virtual uint64_t resourceGeneration() const = 0;
    virtual int frameIndex() const = 0;
};

} // namespace reone::graphics
