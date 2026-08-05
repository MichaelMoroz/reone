/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "buffer.h"
#include "rhi.h"

namespace reone::graphics {

/** The merged scene geometry from which the frame's tracing structure is built. */
struct SceneTracingGeometry {
    BufferView vertices;
    BufferView indices;
    uint32_t vertexCount {0};
    uint32_t opaqueTriangleCount {0};
    uint32_t triangleCount {0};
};

/**
 * A tracing structure built over one frame's merged scene geometry.
 *
 * The implementation owns all backend allocations required to rebuild the
 * scene-wide structure. Its handle is consumed by later tracing operations.
 */
class ITracingStructure {
public:
    virtual ~ITracingStructure() = default;

    virtual TracingStructure handle() const = 0;
    virtual void deinit() = 0;
};

} // namespace reone::graphics
