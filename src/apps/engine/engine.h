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

#include "reone/audio/di/module.h"
#include "reone/game/di/module.h"
#include "reone/game/game.h"
#include "reone/graphics/di/module.h"
#include "reone/graphics/window.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/gui/di/module.h"
#include "reone/input/event.h"
#include "reone/movie/di/module.h"
#include "reone/resource/di/module.h"
#include "reone/scene/di/module.h"
#include "reone/script/di/module.h"
#include "reone/system/di/module.h"

#include "console.h"
#include "options.h"
#include "profiler.h"

#include <filesystem>
#include <deque>
#include <vector>

namespace reone {

class Editor;

class Engine : boost::noncopyable {
public:
    // Defined out of line because Editor is an incomplete type here.
    Engine(Options &options);
    ~Engine();

    friend class Editor;

    void init();
    void deinit();

    /**
     * @return exit code
     */
    int run();

private:
    struct CaptureRequest {
        std::filesystem::path path;
        int count {1};
        int index {0};
    };

    struct FrameStates {
        static constexpr int rendered = 0;
        static constexpr int updating = 1;
        static constexpr int updated = 2;
        static constexpr int rendering = 3;
    };

    Options &_options;

    std::unique_ptr<game::OptionsView> _optionsView;
    std::unique_ptr<graphics::Window> _window;
    std::unique_ptr<graphics::IRenderer> _renderer;

    std::unique_ptr<Clock> _clock;
    std::unique_ptr<SystemModule> _systemModule;
    std::unique_ptr<resource::ResourceModule> _resourceModule;
    std::unique_ptr<graphics::GraphicsModule> _graphicsModule;
    std::unique_ptr<audio::AudioModule> _audioModule;
    std::unique_ptr<movie::MovieModule> _movieModule;
    std::unique_ptr<scene::SceneModule> _sceneModule;
    std::unique_ptr<gui::GUIModule> _guiModule;
    std::unique_ptr<script::ScriptModule> _scriptModule;
    std::unique_ptr<game::GameModule> _gameModule;

    std::unique_ptr<game::ServicesView> _services;
    std::unique_ptr<game::Game> _game;
    std::unique_ptr<Profiler> _profiler;
    std::unique_ptr<Console> _console;
    std::unique_ptr<Editor> _editor;

    std::queue<input::Event> _events;

    struct AutomatedInputEvent {
        int frame {0};
        SDL_Event event {};
    };
    std::vector<AutomatedInputEvent> _automatedInput;
    size_t _nextAutomatedInput {0};

    uint64_t _ticks {0};

    int _frameIndex {0};
    bool _commandsRun {false};
    bool _inFrame {false};
    bool _renderdocTriggered {false};
    bool _graphicsRebuildRequested {false};
    std::deque<std::string> _scriptedCommands;
    int _scriptPauseFrames {0};
    std::optional<CaptureRequest> _captureRequest;
    bool _scriptQuitRequested {false};

    bool _showCursor {true};
    bool _relativeMouseMode {false};

    /**
     * The edit buffer for options that cannot take effect inside a frame.
     *
     * Options classified OptionApply::Reapply change what the pipeline
     * allocates, so a control that wrote them straight through would leave the
     * running frame describing a pipeline that does not exist. They are edited
     * here instead and copied across by applyStagedGraphics, which is also what
     * schedules the rebuild. Live options are not staged: they are written to
     * _options.graphics directly and only the Reapply fields of this copy are
     * ever read, so the two never disagree about anything else.
     *
     * One buffer, shared by the editor's Apply button and the console's
     * "gfx apply", so there is a single path rather than two.
     */
    graphics::GraphicsOptions _stagedGraphics;

    void processEvents(bool &quit);
    void loadInputScript();
    void runCommandsFile(const std::string &path);
    /** Records the GUI through the 2D renderer, in its own rendering scope. */
    void renderFrame(bool &quit);
    void renderVulkanFrame(bool &quit);
    void processScriptedCommands(bool &quit);
    void captureIfRequested(bool &quit);
    void captureFrame(const std::filesystem::path &path);
    std::filesystem::path numberedCapturePath(const CaptureRequest &request) const;
    void dumpTargetsIfRequested();
    void dumpObjectsIfRequested();

    void showCursor(bool show);
    void setRelativeMouseMode(bool relative);
    void requestGraphicsRebuild() { _graphicsRebuildRequested = true; }
    /**
     * Take a requested rebuild, between frames.
     *
     * Deliberately not inside the frame: the editor submits ImGui image handles
     * owned by the scene pipeline while the update slot's ImGui frame is open,
     * so a rebuild taken after that point would free a descriptor the recorded
     * draw data still names. It waits the device idle itself rather than
     * relying on a swapchain recreate to have done so, because a change that
     * leaves the extent alone - the render mode, the anti-aliasing slot - never
     * triggers one.
     */
    void applyGraphicsRebuild();

    // Runtime graphics options. The editor reaches these as a friend; the
    // console reaches them through the commands registered in init.

    graphics::GraphicsOptions &stagedGraphicsOptions() { return _stagedGraphics; }
    /** Names of the staged reapply options that differ from the live ones. */
    std::vector<std::string> stagedGraphicsChanges() const;
    /** Copy the staged reapply options into the live ones and rebuild. */
    void applyStagedGraphics();
    /** Discard staged edits, restoring the running configuration. */
    void revertStagedGraphics();
    /**
     * Set one option by its command-line name, into the live options or the
     * staged copy according to its class.
     *
     * @return a line describing what happened, for the console to print.
     * @throws std::invalid_argument naming the option, on an unknown name or
     *         an unreadable value.
     */
    std::string setGraphicsOption(const std::string &name, const std::string &value);
    void registerGraphicsCommands();

    std::optional<input::Event> eventFromSDLEvent(const SDL_Event &sdlEvent) const;
};

} // namespace reone
