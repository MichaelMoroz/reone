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

namespace reone::graphics {

/** Per-frame uniform storage addressed by dynamic offsets. */
class IUniformRing {
public:
    virtual ~IUniformRing() = default;

    virtual void init(int framesInFlight, uint64_t bytesPerFrame) = 0;
    virtual void deinit() = 0;
    virtual void beginFrame(int frame) = 0;
    virtual uint32_t push(const void *data, uint64_t size) = 0;
    /** Offset of the frame's GlobalUniforms slice, shared by compute dispatches. */
    virtual void setGlobalsOffset(uint32_t offset) = 0;
    virtual uint32_t globalsOffset() const = 0;
    virtual int frame() const = 0;
    virtual uint64_t peakUsage() const = 0;

    template <class T>
    uint32_t push(const T &value) {
        return push(&value, sizeof(T));
    }
};

} // namespace reone::graphics
