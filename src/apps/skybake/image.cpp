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

#include "image.h"

namespace reone::skybake {

namespace {

void appendUint32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t adler32(const std::vector<uint8_t> &data) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t value : data) {
        a = (a + value) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void appendChunk(std::vector<uint8_t> &png, const char type[4], const std::vector<uint8_t> &data) {
    appendUint32(png, static_cast<uint32_t>(data.size()));
    const size_t crcBegin = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    appendUint32(png, crc32(png.data() + crcBegin, png.size() - crcBegin));
}

std::vector<uint8_t> zlibStore(const std::vector<uint8_t> &input) {
    std::vector<uint8_t> out;
    out.reserve(input.size() + input.size() / 65535 * 5 + 8);
    out.push_back(0x78);
    out.push_back(0x01);
    size_t offset = 0;
    while (offset < input.size()) {
        const size_t count = std::min<size_t>(65535, input.size() - offset);
        const bool final = offset + count == input.size();
        out.push_back(final ? 1 : 0);
        const auto length = static_cast<uint16_t>(count);
        const auto inverse = static_cast<uint16_t>(~length);
        out.push_back(static_cast<uint8_t>(length));
        out.push_back(static_cast<uint8_t>(length >> 8));
        out.push_back(static_cast<uint8_t>(inverse));
        out.push_back(static_cast<uint8_t>(inverse >> 8));
        out.insert(out.end(), input.begin() + offset, input.begin() + offset + count);
        offset += count;
    }
    appendUint32(out, adler32(input));
    return out;
}

} // namespace

Image::Image(int width, int height, glm::u8vec3 color) :
    width(width),
    height(height),
    pixels(static_cast<size_t>(width) * height * 3) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            setPixel(x, y, color);
}

glm::u8vec3 Image::pixel(int x, int y) const {
    const auto offset = 3ull * (static_cast<size_t>(y) * width + x);
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
}

void Image::setPixel(int x, int y, glm::u8vec3 color) {
    const auto offset = 3ull * (static_cast<size_t>(y) * width + x);
    pixels[offset] = color.r;
    pixels[offset + 1] = color.g;
    pixels[offset + 2] = color.b;
}

void writePng(const std::filesystem::path &path, const Image &image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.pixels.size() != static_cast<size_t>(image.width) * image.height * 3)
        throw std::invalid_argument("Invalid RGB image");

    std::vector<uint8_t> scanlines;
    scanlines.reserve(static_cast<size_t>(image.height) * (1 + 3 * image.width));
    for (int y = 0; y < image.height; ++y) {
        scanlines.push_back(0);
        const auto begin = image.pixels.begin() + static_cast<size_t>(y) * image.width * 3;
        scanlines.insert(scanlines.end(), begin, begin + static_cast<size_t>(image.width) * 3);
    }

    std::vector<uint8_t> png {137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr;
    appendUint32(ihdr, static_cast<uint32_t>(image.width));
    appendUint32(ihdr, static_cast<uint32_t>(image.height));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    appendChunk(png, "IHDR", ihdr);
    appendChunk(png, "IDAT", zlibStore(scanlines));
    appendChunk(png, "IEND", {});

    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("Unable to create PNG: " + path.string());
    out.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out)
        throw std::runtime_error("Unable to write PNG: " + path.string());
}

} // namespace reone::skybake
