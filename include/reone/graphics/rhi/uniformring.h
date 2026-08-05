/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>

namespace reone::graphics {

/** Per-frame uniform storage addressed by dynamic offsets. */
class IUniformRing {
public:
    virtual ~IUniformRing() = default;

    virtual void init(int framesInFlight, uint64_t bytesPerFrame) = 0;
    virtual void deinit() = 0;
    virtual void beginFrame(int frame) = 0;
    virtual uint32_t push(const void *data, uint64_t size) = 0;
    virtual int frame() const = 0;
    virtual uint64_t peakUsage() const = 0;

    template <class T>
    uint32_t push(const T &value) {
        return push(&value, sizeof(T));
    }
};

} // namespace reone::graphics
