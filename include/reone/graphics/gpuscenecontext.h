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
