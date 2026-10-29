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

#include "reone/scene/skyrooms.h"

#include "reone/system/logutil.h"

namespace reone::scene {

void SkyRooms::load(const std::filesystem::path &path) {
    _rooms.clear();
    std::ifstream input(path);
    if (!input) {
        // Not an error. A game with no curated list simply has no sky rooms,
        // which is the same answer the file gives for a module it does not
        // name.
        info("Sky rooms: no curated list at " + path.string(), LogChannel::Graphics);
        return;
    }
    std::string line;
    while (std::getline(input, line)) {
        // Comments carry the survey's rejected candidates in the same
        // `room=name` shape as a real entry, so stripping them first is what
        // keeps a near miss from being read as a decision.
        if (auto comment = line.find('#'); comment != std::string::npos) {
            line.resize(comment);
        }
        boost::trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            // Section headings scope the file for a reader; the room names
            // below are unique on their own, which is what the lookup uses.
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        auto key = boost::to_lower_copy(line.substr(0, equals));
        boost::trim(key);
        // `sky` names the baked asset and `meshes` narrows the shell; neither
        // is needed to decide which room the sky is, which is all this answers.
        if (key != "room") {
            continue;
        }
        auto value = boost::to_lower_copy(line.substr(equals + 1));
        boost::trim(value);
        // `sky = none` is the survey's way of saying a module has no sky, and
        // it is written with the `sky` key, so it never reaches here. A `room`
        // key naming nothing would be a malformed entry rather than a verdict.
        if (!value.empty()) {
            _rooms.insert(value);
        }
    }
    info("Sky rooms: " + std::to_string(_rooms.size()) + " curated from " +
             path.filename().string(),
         LogChannel::Graphics);
}

bool SkyRooms::isSkyRoom(const std::string &roomName) const {
    return _rooms.count(boost::to_lower_copy(roomName)) > 0;
}

} // namespace reone::scene
