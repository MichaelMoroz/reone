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
