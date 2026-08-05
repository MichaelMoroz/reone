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
#include <gtest/gtest.h>

#include "reone/scene/render/pipeline/tracematerials.h"

using namespace reone::scene;

TEST(TraceMaterialOverrides, should_preserve_trace_class_values_and_round_trip) {
    EXPECT_EQ(0, static_cast<int>(TraceClass::Default));
    EXPECT_EQ(1, static_cast<int>(TraceClass::Prelit));
    EXPECT_EQ(2, static_cast<int>(TraceClass::Emissive));
    EXPECT_EQ(3, static_cast<int>(TraceClass::None));

    auto path = std::filesystem::temp_directory_path() /
                "reone_trace_material_round_trip.txt";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    TraceMaterialOverrides written;
    written.loadTraceClasses(path);
    for (auto [node, klass] : {
             std::pair {"prelit", TraceClass::Prelit},
             std::pair {"emissive", TraceClass::Emissive},
             std::pair {"none", TraceClass::None},
         }) {
        CuratedMaterial material;
        material.klass = klass;
        written.setCurated("roundtrip", node, material);
    }

    TraceMaterialOverrides read;
    read.loadTraceClasses(path);
    EXPECT_EQ(TraceClass::Prelit, read.curatedFor("roundtrip", "prelit").klass);
    EXPECT_EQ(TraceClass::Emissive, read.curatedFor("roundtrip", "emissive").klass);
    EXPECT_EQ(TraceClass::None, read.curatedFor("roundtrip", "none").klass);

    std::filesystem::remove(path, ec);
}
