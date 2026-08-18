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

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "options.h"

namespace reone {

namespace graphics {

/**
 * When a change to a graphics option becomes visible.
 *
 * The classification is a property of the option, not of the place it is
 * edited, so it lives here beside the struct rather than being restated in the
 * editor, the console and the command line. Everything that lets a user change
 * an option at runtime reads this table; nothing decides for itself.
 */
enum class OptionApply {
    /**
     * Read afresh by the frame that follows. Push constants, per-frame plan
     * steps, uniforms, and anything the scene pushes down every update.
     */
    Live,
    /**
     * Changes what the pipeline allocates, so it takes a graphics rebuild:
     * target sizes, the presence of an optional stage, the swapchain. The
     * rebuild destroys the scene pipelines and lets the next frame recreate
     * them, which is the only point at which such a choice can change.
     */
    Reapply,
    /**
     * Consumed once, before anything that could be rebuilt exists - at window
     * creation, or while assets are loaded. Nothing short of relaunching (or,
     * for the texture dials, reloading every asset) makes a change visible, so
     * these are reported rather than pretended to apply.
     */
    Restart,
};

const char *optionApplyName(OptionApply apply);

/**
 * The written form of a render mode - the one the command line, reone.cfg, the
 * console and the launcher all use.
 */
const char *renderModeName(RenderMode mode);
const char *grassModeName(GrassMode mode);

/**
 * Read a render mode from that written form.
 *
 * "raster" is accepted as a spelling of retro. It was this option's old value
 * for the whole raster family, chosen between by a separate pbr flag that no
 * longer exists; keeping the spelling means an existing script or config still
 * names a mode rather than failing to launch.
 *
 * @throws std::invalid_argument naming the offending text.
 */
RenderMode parseRenderMode(const std::string &value);
GrassMode parseGrassMode(const std::string &value);

/**
 * One named graphics option: how to read it, how to parse it, and how to tell
 * two option sets apart on it.
 *
 * The name is the command-line flag's name from optionsparser.cpp, so one
 * vocabulary spans the command line, reone.cfg, the console and the editor.
 */
struct GraphicsOptionDesc {
    std::string name;
    /**
     * How a change to this option applies, when that does not depend on the
     * value. Where it does, this is the worst case and @ref applyFor decides.
     */
    OptionApply apply {OptionApply::Live};
    /**
     * The class of a change from one option set to another, when the option has
     * values that differ in what changing them costs.
     *
     * The render mode is the case that needs it: retro and PBR pick a resolve
     * step per frame over targets that already exist, while path tracing
     * decides whether the tracer exists at all. One option, two costs, decided
     * per value rather than per option - and nothing that edits an option needs
     * to know which option this is, only to ask.
     *
     * Null for every option whose class is a property of the option alone.
     * Must be symmetric in its arguments: callers pass them in either order.
     */
    std::function<OptionApply(const GraphicsOptions &, const GraphicsOptions &)> applyFor;
    std::string help;
    /** The value in the same written form the command line and reone.cfg use. */
    std::function<std::string(const GraphicsOptions &)> get;
    /** Throws std::invalid_argument, naming the option and the offending text. */
    std::function<void(GraphicsOptions &, const std::string &)> set;
    std::function<bool(const GraphicsOptions &, const GraphicsOptions &)> equal;
    std::function<void(const GraphicsOptions &, GraphicsOptions &)> copy;
};

/** Every option, in the order the command line declares them. */
const std::vector<GraphicsOptionDesc> &graphicsOptionDescs();

/** Null when no option carries that name; callers must say so out loud. */
const GraphicsOptionDesc *findGraphicsOptionDesc(const std::string &name);

/**
 * The class of a change to @p desc between these two option sets.
 *
 * Ask this rather than reading GraphicsOptionDesc::apply: a value-dependent
 * option answers "live" for one pair of values and "requires reapply" for
 * another, and a caller that read the field would show the wrong hint for one
 * of them.
 */
OptionApply graphicsOptionApply(const GraphicsOptionDesc &desc,
                                const GraphicsOptions &left,
                                const GraphicsOptions &right);

/** Names of the options of class @p apply on which @p left and @p right differ. */
std::vector<std::string> graphicsOptionsDiffering(const GraphicsOptions &left,
                                                  const GraphicsOptions &right,
                                                  OptionApply apply);

/** Copy just the options of class @p apply from @p from into @p to. */
void copyGraphicsOptions(const GraphicsOptions &from, GraphicsOptions &to,
                         OptionApply apply);

} // namespace graphics

} // namespace reone
