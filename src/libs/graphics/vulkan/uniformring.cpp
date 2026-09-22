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

#include "reone/graphics/vulkan/uniformring.h"

#include "reone/graphics/vulkan/device.h"

namespace reone {

namespace graphics {

void VulkanUniformRing::init(int framesInFlight, uint64_t bytesPerFrame) {
    _capacity = static_cast<VkDeviceSize>(bytesPerFrame);
    _arenas.reserve(framesInFlight);
    for (int i = 0; i < framesInFlight; ++i) {
        auto arena = std::make_unique<VulkanBuffer>(_device);
        arena->initHostVisible(bytesPerFrame, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        _arenas.push_back(std::move(arena));
    }
}

void VulkanUniformRing::deinit() {
    _arenas.clear();
    _capacity = 0;
    _offset = 0;
    _globalsOffset = 0;
}

void VulkanUniformRing::beginFrame(int frame) {
    _frame = frame;
    _offset = 0;
    _globalsOffset = 0;
}

uint32_t VulkanUniformRing::push(const void *data, uint64_t size) {
    auto offset = _offset;
    if (offset + size > _capacity) {
        // Deliberately fatal. Wrapping would hand out storage that draws
        // already recorded this frame are still pointing at, and the resulting
        // corruption would look like a shader bug rather than an allocation
        // one. Raise the arena size instead - peakUsage() says what it needs.
        throw std::runtime_error(
            str(boost::format("Vulkan: uniform arena exhausted (%llu of %llu bytes used, "
                              "%llu more requested)") %
                offset % _capacity % size));
    }
    std::memcpy(static_cast<uint8_t *>(_arenas[_frame]->mapped()) + offset,
                data, static_cast<size_t>(size));

    // The next slice must start where the device is willing to bind.
    _offset = _device.alignUniform(offset + size);
    _peak = std::max(_peak, _offset);
    return static_cast<uint32_t>(offset);
}

} // namespace graphics

} // namespace reone
