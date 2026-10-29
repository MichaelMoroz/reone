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
#include <string_view>
#include <vector>

#include <glm/vec3.hpp>

#include "reone/graphics/rendering/scenepipeline.h"

namespace reone {

/**
 * Scoped immediate-mode debug primitives, drawn through the debug overlay.
 *
 * The namespace below is UPSTREAM'S, declaration for declaration, so callers
 * that use it - the pathfinder narrates its funnels and face graph this way -
 * compile here byte-identical and merge without conflict. Keep it that way:
 * the point of this file is that the interesting divergence lives elsewhere.
 *
 * What differs is only where the elements go. Upstream renders them from an
 * immediate-mode pass that built vertices per call; that pass was deleted in
 * d6148ee66 and is not coming back. Here they are collected into the same
 * DebugOverlay the object boxes and name labels already use, which buys one
 * draw path, one depth image, and identical occlusion between a pathfinder
 * line and an object's bounding box. collectDrawDebug replaces upstream's
 * renderDrawDebug for that reason, and is the one declaration in this file
 * that can ever conflict.
 */
namespace drawdebug {
/**
 * DrawDebug ID opens a new scope for elements to go to.
 *
 * All operations (such as clear(), line(), box(), etc.) operate implicitly on
 * the list of elements of the current scope.
 */
void pushId(uint64_t id);

/**
 * Convenience wrapper to use any pointer as an ID.
 */
void pushId(const void *id);

/**
 * Close the current scope.
 */
void popId();

/**
 * Set lifetime (in seconds) for all subsequent elements. Elements are removed
 * by updateDrawDebug once their lifetime is over, or when clear() is called
 * explicitly.
 */
void pushLifetime(float lifetime);

/**
 * Set the previous lifetime value.
 */
void popLifetime();

/**
 * Set scene graph name for all subsequent elements.
 */
void pushScene(std::string sceneName);

/**
 * Set the previous scene graph.
 */
void popScene();

/**
 * Remove all elements in the current scope (which is defined by pushId()).
 */
void clear();

void line(glm::vec3 start, glm::vec3 end, uint32_t colorRgba, float thickness);
void triangle(glm::vec3 v0, glm::vec3 v1, glm::vec3 v2, uint32_t colorRgba);
void text(const std::string &str, glm::vec3 position, uint32_t colorRgba, float scale = 1.0);
void point(glm::vec3 position, uint32_t colorRgba, float scale = 1.0);
void box(glm::vec3 min, glm::vec3 max, uint32_t colorRgba);

} // namespace drawdebug

/** Age every element and drop what has outlived its pushLifetime. */
void updateDrawDebug(float dt);

/** Drop every element in every scope, whatever its lifetime. */
void clearDrawDebug();

/**
 * Append this scene's live elements to the overlay buffers.
 *
 * Lines, triangles and points become overlay lines; boxes become overlay
 * shapes; text becomes overlay labels. Appending rather than assigning is what
 * lets object boxes and debug primitives share one pass - see
 * SceneGraph::collectDebugOverlay, the only caller.
 */
void collectDrawDebug(std::string_view sceneName,
                      std::vector<graphics::DebugOverlayShape> &shapes,
                      std::vector<graphics::DebugOverlayLine> &lines,
                      std::vector<graphics::DebugOverlayLabel> &labels);

} // namespace reone
