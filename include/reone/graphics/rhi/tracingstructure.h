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
    /**
     * Procedural sprites at the END of the non-opaque range, which the BLAS
     * does not build.
     *
     * Rejecting them inside the candidate loop still paid for descending to
     * them, intersecting them and loading their material first, which on a
     * plume of overlapping camera-facing quads is most of the cost. Leaving
     * them out of the structure is what makes them free. The tail position is
     * load-bearing: a candidate maps back through geometryBase1 + primitive,
     * so dropping a suffix shifts nothing, and dropping anything else would
     * silently repoint every triangle after it.
     */
    uint32_t spriteTriangleCount {0};

    /**
     * Grass cards, which are instanced rather than merged.
     *
     * A card is one of four fitted templates under a per-card affine
     * transform, so the same handful of triangles is placed thousands of
     * times. Merging them would write every placement out as its own geometry
     * and rebuild all of it every frame; as template plus transform, the
     * templates are built once and only the instance list is per-frame.
     *
     * The templates are shared with the rasterizer, which draws the same
     * vertices through the same transforms - that shared derivation is what
     * keeps the traced scene agreeing with the rasterized one that owns
     * primary visibility.
     */
    BufferView cardVertices;
    BufferView cardIndices;
    /** Per-card transforms, as GrassCardInstance - see rendering/gpuscene.h. */
    BufferView cardInstances;
    uint32_t cardCount {0};
    uint32_t cardVertexCount {0};
    uint32_t cardTriangleCount {0};
    /**
     * Bumped when the fitted templates change, which is the only thing the
     * card geometry depends on. The templates are rebuilt on that change and
     * never per frame.
     */
    uint64_t cardGeneration {0};
    /** TLAS records: one merged record followed by four fixed card regions. */
    IBuffer *instances {nullptr};
    uint32_t instanceCount {0};
    uint32_t cardRegionCapacity {0};
    uint64_t instanceGeneration {0};
};

/** Fitted card templates, one per grass variant - see graphics/grasscard.h. */
constexpr uint32_t kGrassCardVariants = 4;

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
