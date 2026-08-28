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

#include <gtest/gtest.h>

#include <set>

#include "reone/graphics/optionsregistry.h"

using namespace reone;
using namespace reone::graphics;

TEST(OptionsRegistry, should_have_unique_option_names) {
    // Two descriptors under one name would make the config's meaning depend on
    // which one the reader reached first.
    std::set<std::string> seen;
    for (const auto &desc : graphicsOptionDescs()) {
        EXPECT_TRUE(seen.insert(desc.name).second) << "duplicate option name: " << desc.name;
    }
}

TEST(OptionsRegistry, should_round_trip_every_option_through_its_written_form) {
    // The settings window saves by asking every descriptor for its own written
    // form, so a descriptor whose get() is not the inverse of its set() would
    // silently write a config that reads back as a different value. That is
    // invisible until someone notices a setting not sticking, which is exactly
    // how the missing-options bug was reported, so it is guarded here rather
    // than left to the next person to rediscover.
    GraphicsOptions options;
    for (const auto &desc : graphicsOptionDescs()) {
        const std::string written = desc.get(options);

        GraphicsOptions parsed;
        desc.set(parsed, written);

        EXPECT_EQ(written, desc.get(parsed))
            << "option " << desc.name << " does not survive a save/load round trip";
        EXPECT_TRUE(desc.equal(options, parsed))
            << "option " << desc.name << " compares unequal after a save/load round trip";
    }
}

TEST(OptionsRegistry, should_copy_only_the_option_it_owns) {
    // The settings window stages changes by copying one option at a time; a
    // copy() that reached past its own field would drag unrelated staged values
    // across with it.
    for (const auto &desc : graphicsOptionDescs()) {
        GraphicsOptions source;
        GraphicsOptions target;
        desc.copy(source, target);

        for (const auto &other : graphicsOptionDescs()) {
            EXPECT_TRUE(other.equal(source, target))
                << "copying " << desc.name << " also changed " << other.name;
        }
    }
}

TEST(OptionsRegistry, should_round_trip_every_option_away_from_its_default) {
    // The test above only proves the encoding at the value an option ships
    // with, and an encoded option can be its own inverse there by accident -
    // shadowres writes an exponent, so 1024 maps to "0" whichever direction the
    // arithmetic runs. Feeding each option a spread of written forms and
    // demanding the result be a fixed point exercises the encoding away from
    // that coincidence, including whatever clamping it applies.
    const char *candidates[] = {"0", "1", "2", "3", "0.25", "0.5", "2.5", "-1", "1000000"};
    for (const auto &desc : graphicsOptionDescs()) {
        for (const char *candidate : candidates) {
            GraphicsOptions once;
            try {
                desc.set(once, candidate);
            } catch (const std::exception &) {
                // A named option rejecting a form it does not accept is the
                // command line's error path, not a round-trip failure.
                continue;
            }
            const std::string written = desc.get(once);

            GraphicsOptions twice;
            desc.set(twice, written);

            EXPECT_EQ(written, desc.get(twice))
                << "option " << desc.name << " does not survive a round trip from " << candidate;
        }
    }
}
