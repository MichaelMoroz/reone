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

#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/resource/container/memory.h"
#include "reone/resource/provider/textures.h"
#include "reone/resource/resources.h"
#include "reone/system/stringbuilder.h"

using namespace reone;
using namespace reone::graphics;
using namespace reone::resource;

TEST(Textures, should_expose_animated_grayscale_bump_map_as_array) {
    auto tga = StringBuilder()
                   .append("\x00", 1)
                   .append("\x00", 1)
                   .append("\x03", 1)
                   .append('\x00', 9)
                   .append("\x02\x00", 2)
                   .append("\x02\x00", 2)
                   .append("\x08", 1)
                   .append("\x00", 1)
                   .append("\x10\x20\x30\x40", 4)
                   .string();
    auto txi = std::string("proceduretype cycle\nnumx 2\nnumy 2\n");

    auto resources = Resources();
    auto provider = std::make_unique<MemoryResourceContainer>();
    provider->add(ResourceId("bump", ResType::Tga), ByteBuffer(tga.begin(), tga.end()));
    provider->add(ResourceId("bump", ResType::Txi), ByteBuffer(txi.begin(), txi.end()));
    resources.add(std::move(provider));

    auto options = GraphicsOptions();
    auto textures = Textures(options, resources);
    auto texture = textures.get("bump", TextureUsage::BumpMap);

    ASSERT_TRUE(texture);
    EXPECT_TRUE(texture->is2DArray());
    EXPECT_EQ(1, texture->width());
    EXPECT_EQ(1, texture->height());
    EXPECT_EQ(4u, texture->layers().size());
}
