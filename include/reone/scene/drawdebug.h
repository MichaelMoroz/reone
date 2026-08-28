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

#include <cstdint>
#include <string>

#include <glm/vec3.hpp>

namespace reone {

/**
 * The immediate-mode debug draw the pathfinder narrates itself with, reduced to
 * nothing on this backend.
 *
 * The real facility was deleted with the rest of the immediate-mode drawing in
 * d6148ee66 ("the frame speaks Vulkan from birth"): it built vertices per call
 * and issued its own draws, which is the one thing the merged pipeline no
 * longer has a place for. Nothing has replaced it yet.
 *
 * This exists so upstream's pathfinder compiles here BYTE-IDENTICAL. Every
 * entry point is inline and empty, so the calls cost nothing and vanish
 * entirely; the alternative was editing `#if`s into a file the next merge from
 * upstream would have to conflict on. What is lost is the visualisation
 * `setShowPath` used to produce - the pathfinding itself is unaffected, since
 * none of these calls feeds a result back.
 *
 * Implementing it for real means an accumulating line/text buffer flushed
 * through the 2D renderer, which is a piece of work rather than a shim.
 */
namespace drawdebug {

inline void pushId(uint64_t) {}
inline void pushId(const void *) {}
inline void popId() {}

inline void pushLifetime(float) {}
inline void popLifetime() {}

inline void pushScene(std::string) {}
inline void popScene() {}

inline void clear() {}

inline void line(const glm::vec3 &, const glm::vec3 &, uint32_t, float) {}
inline void text(std::string, const glm::vec3 &, uint32_t) {}

} // namespace drawdebug

} // namespace reone
