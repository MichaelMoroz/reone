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

/** The element type of an array written to .npy. */
enum class NpyType {
    UInt8,
    UInt16,
    UInt32,
    Float16,
    Float32
};

/**
 * Write a height x width x channels array to a NumPy .npy file.
 *
 * Chosen over an image format because a render target is measurement data
 * rather than a picture: depth is 32-bit float, motion is signed half float,
 * and neither survives a trip through PNG. .npy carries shape and element type
 * with the bytes, so `numpy.load` on the other side gets the array back exactly
 * as it left the GPU - which is the whole point of dumping it for comparison.
 *
 * @param data tightly packed, row-major, first row first.
 */
void writeNpy(const std::filesystem::path &path,
              const void *data,
              int width,
              int height,
              int channels,
              NpyType type);

} // namespace graphics

} // namespace reone
