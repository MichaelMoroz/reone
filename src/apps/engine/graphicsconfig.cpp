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

#include "graphicsconfig.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

#include "reone/graphics/optionsregistry.h"

namespace reone {

namespace {

constexpr char kConfigFilename[] = "reone.cfg";

std::string formatConfigFloat(float value) {
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return stream.str();
}

} // namespace

bool saveGraphicsOptions(const graphics::GraphicsOptions &options, std::string &error) {
    // Every option the registry knows, asked for its own written form.
    //
    // This was a hand-maintained literal and it had drifted: 19 of the
    // registry's 114 options were absent, so changing one of them in this
    // window and pressing Save wrote nothing and still reported success -
    // indistinguishable from a save that does not work, which is how it was
    // reported. Among the missing were every gui*scale, shadows, particles,
    // lightmaps and several path-tracing dials.
    //
    // Driving it from the registry cannot drift: an option that exists is
    // written, because the descriptor that defines it also supplies its value.
    // The command line and reone.cfg already read through this same list, so
    // all three now agree on what an option is called and how it is spelled.
    std::vector<std::pair<std::string, std::string>> values;
    values.reserve(graphics::graphicsOptionDescs().size());
    for (const auto &desc : graphics::graphicsOptionDescs()) {
        // headless is a launch mode, not a preference: writing it would let a
        // scripted capture run leave the next interactive start windowless.
        if (desc.name == "headless") {
            continue;
        }
        values.emplace_back(desc.name, desc.get(options));
    }

    for (int i = 0; i < 9; ++i) {
        const auto &override = options.categoryOverrides[i];
        auto key = "cat" + std::to_string(i);
        values.emplace_back(key + "color0", formatConfigFloat(override.color[0]));
        values.emplace_back(key + "color1", formatConfigFloat(override.color[1]));
        values.emplace_back(key + "color2", formatConfigFloat(override.color[2]));
        values.emplace_back(key + "colorweight", formatConfigFloat(override.colorWeight));
        values.emplace_back(key + "roughness", formatConfigFloat(override.roughness));
        values.emplace_back(key + "roughnessscale", formatConfigFloat(override.roughnessScale));
        values.emplace_back(key + "emission", formatConfigFloat(override.emissionScale));
        values.emplace_back(key + "env", formatConfigFloat(override.envScale));
        values.emplace_back(key + "metallic", formatConfigFloat(override.metallicScale));
    }

    std::ifstream input(kConfigFilename);
    if (!input && std::filesystem::exists(kConfigFilename)) {
        error = "Could not read reone.cfg.";
        return false;
    }

    // Launcher, game and editor settings share this file, so only owned keys
    // are replaced while every foreign line keeps its position and contents.
    std::vector<std::string> lines;
    std::set<std::string> written;
    for (std::string line; std::getline(input, line);) {
        auto separator = line.find('=');
        auto key = separator == std::string::npos ? std::string() : line.substr(0, separator);
        auto value = std::find_if(values.begin(), values.end(), [&key](const auto &entry) { return entry.first == key; });
        if (value == values.end()) {
            lines.push_back(std::move(line));
        } else if (written.insert(key).second) {
            lines.push_back(value->first + "=" + value->second);
        }
    }

    for (const auto &[key, value] : values) {
        if (written.insert(key).second) {
            lines.push_back(key + "=" + value);
        }
    }

    std::ofstream output(kConfigFilename);
    if (!output) {
        error = "Could not write reone.cfg.";
        return false;
    }
    for (const auto &line : lines) {
        output << line << '\n';
    }
    if (!output) {
        error = "Could not finish writing reone.cfg.";
        return false;
    }
    return true;
}

} // namespace reone
