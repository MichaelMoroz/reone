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

#include "device.h"

namespace reone {

namespace graphics {

/**
 * A labelled region in a command buffer, closed when it goes out of scope.
 *
 * This is what turns a flat list of a few hundred draws in a graphics debugger
 * into something with the shape of the frame in it - shadows, geometry,
 * resolve, transparency. Scoped rather than paired calls because an early
 * return that skipped the end would corrupt the nesting for the whole frame,
 * and the passes here return early routinely when a callback is absent.
 */
class VulkanDebugScope : boost::noncopyable {
public:
    VulkanDebugScope(const VulkanDevice &device,
                     VkCommandBuffer cmd,
                     const char *name,
                     const glm::vec3 &color = {0.4f, 0.6f, 0.9f}) :
        _device(device),
        _cmd(cmd) {
        _device.beginLabel(_cmd, name, color);
    }

    ~VulkanDebugScope() {
        _device.endLabel(_cmd);
    }

private:
    const VulkanDevice &_device;
    VkCommandBuffer _cmd;
};

} // namespace graphics

} // namespace reone
