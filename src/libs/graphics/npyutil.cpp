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

#include "reone/graphics/npyutil.h"

namespace reone {

namespace graphics {

static const char *descrFor(NpyType type) {
    // Little-endian throughout; every platform this builds on is.
    switch (type) {
    case NpyType::UInt8:
        return "|u1";
    case NpyType::UInt16:
        return "<u2";
    case NpyType::Float16:
        return "<f2";
    case NpyType::Float32:
        return "<f4";
    default:
        throw std::invalid_argument("npy: unknown element type");
    }
}

static size_t sizeOf(NpyType type) {
    switch (type) {
    case NpyType::UInt8:
        return 1;
    case NpyType::UInt16:
        return 2;
    case NpyType::Float16:
        return 2;
    case NpyType::Float32:
        return 4;
    default:
        throw std::invalid_argument("npy: unknown element type");
    }
}

void writeNpy(const std::filesystem::path &path,
              const void *data,
              int width,
              int height,
              int channels,
              NpyType type) {
    std::ostringstream dict;
    dict << "{'descr': '" << descrFor(type) << "', 'fortran_order': False, 'shape': ("
         << height << ", " << width << ", " << channels << "), }";
    auto header = dict.str();

    // The format requires the header to be padded so that the data that follows
    // starts on a 64-byte boundary, and to end with a newline.
    static constexpr size_t kPreambleSize = 10; // magic, version, header length
    size_t unpadded = kPreambleSize + header.size() + 1;
    size_t padding = (64 - (unpadded % 64)) % 64;
    header.append(padding, ' ');
    header.push_back('\n');

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("npy: cannot open " + path.string());
    }
    out.write("\x93NUMPY", 6);
    const char version[2] = {1, 0};
    out.write(version, 2);
    uint16_t headerLen = static_cast<uint16_t>(header.size());
    char lenBytes[2] = {static_cast<char>(headerLen & 0xff),
                        static_cast<char>((headerLen >> 8) & 0xff)};
    out.write(lenBytes, 2);
    out.write(header.data(), header.size());
    out.write(reinterpret_cast<const char *>(data),
              static_cast<std::streamsize>(sizeOf(type) * width * height * channels));
}

} // namespace graphics

} // namespace reone
