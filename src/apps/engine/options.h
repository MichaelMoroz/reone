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
     * Run the commands file on this frame instead of during init.
     *
     * Loading a module from a running game is not the same code path as warping
     * before the first frame: the previous scene is torn down while the
     * renderer holds resources from it. This makes that reproducible.
     */
    int commandsFrame {0};
    /** Frame-indexed SDL mouse input script for deterministic UI automation. */
    std::string inputScript;
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
    int captureFrame {3};
    /**
     * Capture this many consecutive frames ending the run, rather than one.
     * Each gets its frame number appended to the stem, so capturePath
     * "out.tga" with captureFrame 350 writes out_0350.tga, out_0351.tga...
     *
     * A single frame says nothing about whether a temporal filter converges.
     * A run of them does: with the simulation frozen, the only thing still
     * moving is the accumulation, so the difference between consecutive
     * frames is the residual the denoiser and TAA have not yet removed, and
     * it must fall towards zero.
     */
    int captureFrames {1};
    /**
     * Stop advancing the simulation from this frame on, or 0 not to.
     *
     * Rendering continues untouched - the jitter sequence, the tracer's frame
     * index, NRD's accumulation and the TAA history all keep advancing - while
     * the camera, animations and AI hold still. That separates temporal
     * convergence from scene motion, which is the only way to say whether a
     * residual is the filter failing or the world moving under it.
     */
    int freezeFrame {0};

    /**
     * Seed for the shared random generator, or -1 to seed from the wall clock.
     * A capture run seeds deterministically unless told otherwise, for the same
     * reason it uses a fixed timestep: an unrepeatable run cannot be compared.
     */
    int randomSeed {-1};

    /** Vulkan validation layers. Off by default; they cost real time. */
    bool vulkanValidation {false};
    /** Trigger a RenderDoc frame capture alongside the screenshot. */
    bool renderdoc {false};

    std::unique_ptr<game::OptionsView> toView() {
        return std::make_unique<game::OptionsView>(game, graphics, audio);
    }
};

} // namespace reone
