/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include <gtest/gtest.h>

#include "reone/graphics/format/tgareader.h"
#include "reone/graphics/texture.h"
#include "reone/system/stream/fileinput.h"

#include <filesystem>

using namespace reone;
using namespace reone::graphics;

/**
 * The tracer indexes the atlas by arithmetic on its dimensions, so a shipped
 * file that does not decode to exactly 512 square is a silently wrong field
 * rather than a failure.
 */
TEST(BlueNoise, atlas_decodes_to_the_geometry_the_tracer_assumes) {
    const std::filesystem::path path =
        std::filesystem::path(REONE_ASSET_SOURCE_DIR) / "bluenoise_rgba_64x64x64.tga";
    ASSERT_TRUE(std::filesystem::is_regular_file(path)) << path.string();

    FileInputStream stream {path};
    TgaReader reader {stream, "bluenoise", TextureUsage::Noise};
    reader.load();

    auto texture = reader.texture();
    ASSERT_TRUE(static_cast<bool>(texture));
    EXPECT_EQ(512, texture->width());
    EXPECT_EQ(512, texture->height());
    EXPECT_EQ(PixelFormat::BGRA8, texture->pixelFormat());
    ASSERT_EQ(1u, texture->layers().size());
    EXPECT_EQ(512u * 512u * 4u, texture->layers().front().pixels->size());
}
