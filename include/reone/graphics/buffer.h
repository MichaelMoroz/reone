/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>

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
    /** Replace a range of immutable storage before the next merge. */
    virtual void uploadDeviceStorage(uint64_t offset, uint64_t size, const void *data) = 0;
    virtual void deinit() = 0;

    virtual uint64_t size() const = 0;
    virtual void *mapped() const = 0;
};

/** A byte range of storage used as one input or output of the scene merge. */
struct BufferView {
    const IBuffer *buffer {nullptr};
    uint64_t offset {0};
    uint64_t size {0};
};

} // namespace reone::graphics
