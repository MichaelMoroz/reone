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

#include "rhi.h"

namespace reone::graphics {

/** Storage owned by a GPU scene. The roles reflect the data it publishes, not
 * allocation or API usage flags. */
class IBuffer {
public:
    virtual ~IBuffer() = default;

    /** CPU-written storage consumed by the scene merge compute pass. */
    virtual void initHostVisibleStorage(uint64_t size) = 0;
    /** Immutable storage consumed by the scene merge compute pass. */
    virtual void initDeviceStorage(uint64_t size, const void *data) = 0;
    /** Storage written by the merge and consumed as scene geometry. */
    virtual void initMergedGeometry(uint64_t size) = 0;
    /** Host-visible storage written by the GPU and read by the CPU next frame. */
    virtual void initHostVisibleReadback(uint64_t size) = 0;
    /** Replace a range of immutable storage before the next merge. */
    virtual void uploadDeviceStorage(uint64_t offset, uint64_t size, const void *data) = 0;
    virtual void deinit() = 0;

    virtual Buffer rhiHandle() const = 0;
    virtual uint64_t size() const = 0;
    virtual void *mapped() const = 0;
    virtual void invalidateMapped() const = 0;
};

/** A byte range of storage used as one input or output of the scene merge. */
struct BufferView {
    const IBuffer *buffer {nullptr};
    uint64_t offset {0};
    uint64_t size {0};
};

} // namespace reone::graphics
