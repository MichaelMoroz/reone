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

namespace reone {

namespace graphics {

enum class GraphicsBackend {
    OpenGL,
    Vulkan
};

/**
 * Which backend this process is running.
 *
 * Deliberately process-wide rather than threaded through every constructor.
 * OpenGL and Vulkan cannot share a window, so this is chosen once before
 * anything graphical exists and never changes; and the things that need to ask
 * are asset objects like Texture and Mesh, which are created deep inside
 * resource providers that have no reason to know about backends otherwise.
 *
 * Must be set before the window is created.
 */
GraphicsBackend currentBackend();
void setCurrentBackend(GraphicsBackend backend);

inline bool isVulkanBackend() {
    return currentBackend() == GraphicsBackend::Vulkan;
}

} // namespace graphics

} // namespace reone
