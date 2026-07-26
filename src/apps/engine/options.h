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

#pragma once

#include "reone/audio/options.h"
#include "reone/game/options.h"
#include "reone/graphics/options.h"
#include "reone/system/types.h"

namespace reone {

struct Options {
    struct Logging {
        LogSeverity severity {LogSeverity::Info};
        std::set<LogChannel> channels {LogChannel::Global};
    };

    game::GameOptions game;
    graphics::GraphicsOptions graphics;
    audio::AudioOptions audio;

    Logging logging;

    /**
     * Execute console commands from a file at startup.
     */
    std::string commandsFile;
    /**
     * Write a screenshot to this path on frame captureFrame and exit. Lets two
     * builds be rendered and compared without a human in the loop.
     *
     * Counted in frames rather than seconds because animations advance per frame:
     * two runs stopped at the same wall-clock time differ by whatever idle
     * animation, foliage movement and glow have done in between, which shows up
     * in the diff as though it were a rendering difference.
     */
    std::string capturePath;
    int captureFrame {3};

    /**
     * Seed for the shared random generator, or -1 to seed from the wall clock.
     * A capture run seeds deterministically unless told otherwise, for the same
     * reason it uses a fixed timestep: an unrepeatable run cannot be compared.
     */
    int randomSeed {-1};

    /** "gl" or "vulkan". Chosen before the window exists; the two cannot share one. */
    std::string backend {"gl"};

    /** Vulkan validation layers. Off by default; they cost real time. */
    bool vulkanValidation {false};
    /** Trigger a RenderDoc frame capture alongside the screenshot. */
    bool renderdoc {false};

    std::unique_ptr<game::OptionsView> toView() {
        return std::make_unique<game::OptionsView>(game, graphics, audio);
    }
};

} // namespace reone
