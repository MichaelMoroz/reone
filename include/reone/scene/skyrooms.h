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

#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>

namespace reone::scene {

/**
 * Which room in a module is the sky, read from the curated per-game
 * `modules.ini` and nothing else.
 *
 * This replaces a runtime guess that keyed on the K1 convention of omitting a
 * room's walkmesh. TSL does not follow it - it authors a per-mesh
 * background-geometry flag instead, on rooms the player walks around in - so
 * the guess classified no TSL sky at all and the skybox rendered as ordinary
 * lit geometry. A guess that has to be right about two conventions and is
 * right about one is worse than a list, and the list already existed.
 *
 * Being curated, silence is an answer: a module with no entry has no sky room,
 * and that is a statement rather than a gap to be filled by a heuristic. The
 * survey that drafted the file leaves near misses in it as comments, which are
 * not entries - promoting one is a person's decision, taken by editing the
 * file.
 */
class SkyRooms {
public:
    /** Missing or unreadable file leaves every module without a sky room. */
    void load(const std::filesystem::path &path);

    /**
     * Whether this room model is a curated sky, by name and case-insensitively.
     *
     * Keyed by room rather than by module because the caller is an Area, whose
     * name is the area resref and not the module the list is sectioned by -
     * module `danm14ab` holds area `m14ab`. Room model names are unique across
     * a game, so the section headings are documentation for a person here and
     * the room names carry the meaning.
     */
    bool isSkyRoom(const std::string &roomName) const;

    std::size_t size() const { return _rooms.size(); }

private:
    std::unordered_set<std::string> _rooms;
};

} // namespace reone::scene
