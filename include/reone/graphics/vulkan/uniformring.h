/*
 * Copyright (c) 2020-2026 The reone project contributors
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

#include <volk.h>

#include "buffer.h"

namespace reone {

namespace graphics {

class VulkanDevice;

/**
 * Per-draw uniform data, bump-allocated out of one host-visible buffer per
 * frame in flight and addressed by dynamic offset.
 *
 * This is what replaces the OpenGL model described in §3.1 of the plan, where
 * every setLocals overwrote a whole uniform buffer that queued draws still
 * referenced. That is legal in GL only because the driver renames the storage
 * behind the caller; with recorded command buffers there is no such rescue, and
 * the second draw would read the third draw's data.
 *
 * So each draw gets its own slice, and nothing is ever overwritten within a
 * frame. Reuse happens only when the frame's fence says the GPU has finished
 * with it, which is exactly the condition the caller already waits on.
 */
class VulkanUniformRing : boost::noncopyable {
public:
    VulkanUniformRing(VulkanDevice &device) :
        _device(device) {
    }

    /**
     * @param framesInFlight one independent arena per frame that can be in
     *                       flight at once
     * @param bytesPerFrame  arena size; exceeding it in one frame throws rather
     *                       than wrapping, because wrapping would silently
     *                       corrupt draws already recorded this frame
     */
    void init(int framesInFlight, VkDeviceSize bytesPerFrame);
    void deinit();

    /** Hand the arena for @p frame back to the allocator. */
    void beginFrame(int frame);

    /**
     * Copy @p size bytes into the current arena and return the offset to bind
     * at. The offset is aligned to the device's minimum uniform alignment.
     */
    uint32_t push(const void *data, VkDeviceSize size);

    /** Convenience for the uniform structs, which are all trivially copyable. */
    template <class T>
    uint32_t push(const T &value) {
        return push(&value, sizeof(T));
    }

    /** The frame beginFrame was last called with. */
    int frame() const { return _frame; }

    VkBuffer buffer(int frame) const { return _arenas[frame]->handle(); }
    VkBuffer currentBuffer() const { return _arenas[_frame]->handle(); }

    /** High-water mark across the run, for sizing the arena honestly. */
    VkDeviceSize peakUsage() const { return _peak; }

private:
    VulkanDevice &_device;

    std::vector<std::unique_ptr<VulkanBuffer>> _arenas;
    VkDeviceSize _capacity {0};
    VkDeviceSize _offset {0};
    VkDeviceSize _peak {0};
    int _frame {0};
};

} // namespace graphics

} // namespace reone
