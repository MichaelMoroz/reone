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
    /** Optional second command file run on commandsFrame after the startup file. */
    std::string commandsFrameScheduledFile;
    /**
     * Run the commands file on this frame instead of during init.
     *
     * Loading a module from a running game is not the same code path as warping
     * before the first frame: the previous scene is torn down while the
     * renderer holds resources from it. This makes that reproducible.
     */
    int commandsFrame {0};
    /** Frame-indexed SDL mouse input script for deterministic UI automation. */
    std::string inputScript;
    /** Where to write a replayable recording of this session's input. */
    std::string recordInput;
    /** Where to write the scene render targets, or empty not to. */
    std::string dumpTargetsPath;
    /**
     * Append the traced-emissive candidates of the loaded module to this file
     * on the capture frame: everything still classified emissive by default,
     * one "module TAB model/node TAB texture TAB selfIllum" line each. Feeds
     * the curation pass - a warp loop over every module collects the game-wide
     * list for name-based classification.
     */
    std::string dumpObjectsPath;
    /**
     * Seed for the shared random generator, or -1 to seed from the wall clock.
     * A capture run seeds deterministically unless told otherwise, for the same
     * reason it uses a fixed timestep: an unrepeatable run cannot be compared.
     */
    int randomSeed {-1};

    /** Vulkan validation layers. Off by default; they cost real time. */
    bool vulkanValidation {false};
    /**
     * Vulkan debug labels without the validation layers, for GPU profilers.
     * Off by default: a screenshot capture splits the frame command buffer
     * mid-frame, and an open label scope does not survive that.
     */
    bool vulkanDebugLabels {false};
    /** Trigger a RenderDoc frame capture alongside the screenshot. */
    bool renderdoc {false};

    std::unique_ptr<game::OptionsView> toView() {
        return std::make_unique<game::OptionsView>(game, graphics, audio);
    }
};

} // namespace reone
