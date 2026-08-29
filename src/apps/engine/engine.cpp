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

#include "engine.h"

#include "SDL3/SDL.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "reone/graphics/rhi/renderer.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "renderdoc_app.h"

#include "reone/graphics/format/tgawriter.h"
#include "reone/graphics/optionsregistry.h"
#include "reone/graphics/window.h"
#include "reone/resource/exception/notfound.h"
#include "reone/resource/gameprobe.h"
#include "reone/system/stringutil.h"
#include "reone/system/profiler.h"
#include "reone/system/randomutil.h"
#include "reone/system/stream/fileoutput.h"

#include "editor.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <variant>

using namespace reone::audio;
using namespace reone::game;
using namespace reone::graphics;
using namespace reone::gui;
using namespace reone::movie;
using namespace reone::resource;
using namespace reone::scene;
using namespace reone::script;

namespace reone {

static const std::string kMainThreadName {"main"};

static constexpr int kProfilerInputTimeIndex = 0;
static constexpr int kProfilerUpdateTimeIndex = 1;
static constexpr int kProfilerRenderGraphicsTimeIndex = 2;
static constexpr int kProfilerRenderAudioTimeIndex = 3;

static bool g_imguiFrameOpen = false;

static void imguiCreateContext() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::GetStyle().FontScaleMain = 1.5f;
}

static IRenderer *g_imguiRenderer = nullptr;

static void imguiInit(IRenderer &renderer) {
    renderer.initImGui();
    g_imguiRenderer = &renderer;
}

/**
 * Feed an event to ImGui and report whether ImGui consumed it.
 *
 * The two capture flags must be applied per event kind rather than together:
 * keyboard navigation keeps WantCaptureKeyboard set for as long as an ImGui
 * window holds focus, so testing both would swallow mouse input across the whole
 * screen while any editor window is open.
 */
static bool imguiHandle(SDL_Event &event) {
    ImGuiIO &io = ImGui::GetIO();
    ImGui_ImplSDL3_ProcessEvent(&event);
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return io.WantCaptureMouse;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
        return io.WantCaptureKeyboard;
    default:
        return false;
    }
}

/**
 * Where a replay says the mouse is, or unset.
 *
 * Written into ImGui after NewFrame rather than into the OS. ImGui's SDL3
 * backend re-reads the real cursor in NewFrame whenever the window has focus and
 * would otherwise overwrite the injected position; writing it here wins for the
 * frame and leaves the developer's pointer alone. Moving the physical cursor to
 * make a replay work is never the answer - it takes the machine hostage for the
 * length of the run.
 */
static std::optional<ImVec2> g_replayMousePos;

static void imguiBeginFrame() {
    // Loading can request a frame before the main loop, while normal frames
    // begin before update so widgets submitted there belong to the render that
    // follows. Either path may reach the frame owner first.
    if (g_imguiFrameOpen) {
        return;
    }
    g_imguiRenderer->beginImGuiFrame();
    // Between the backend's NewFrame and ImGui's own, deliberately. The backend
    // has just overwritten MousePos with the real cursor, and ImGui::NewFrame is
    // where a click's position is latched - so setting it after NewFrame fixes
    // hovering but leaves every replayed click aimed at wherever the physical
    // pointer happened to be.
    if (g_replayMousePos) {
        ImGui::GetIO().MousePos = *g_replayMousePos;
    }
    ImGui::NewFrame();
    g_imguiFrameOpen = true;
    if (!ImGui::GetIO().WantCaptureMouse) {
        // Hand the cursor back to the game once it leaves an ImGui window.
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
}

static void imguiRender() {
    ImGui::Render();
    g_imguiFrameOpen = false;
    g_imguiRenderer->renderImGui(*ImGui::GetDrawData());
}

static void imguiShutdown() {
    // deinit runs from the destructor, and also after a failed init, so this
    // must tolerate being called when no context was ever created.
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    if (g_imguiFrameOpen) {
        ImGui::EndFrame();
        g_imguiFrameOpen = false;
    }
    g_imguiRenderer->deinitImGui();
    g_imguiRenderer = nullptr;
}

Engine::Engine(Options &options) :
    _options(options) {
}

Engine::~Engine() {
    deinit();
}

void Engine::init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error("SDL_Init failed: " + std::string(SDL_GetError()));
    }
    _window = std::make_unique<Window>(_options.graphics);
    _window->init();

    imguiCreateContext();

    if (_options.randomSeed >= 0) {
        setRandomSeed(static_cast<uint32_t>(_options.randomSeed));
    } else if (_options.graphics.headless) {
        setRandomSeed(0);
    }

    _optionsView = _options.toView();
    GameProbe probe {_options.game.path};
    auto gameId = probe.probe();

    _clock = std::make_unique<Clock>();
    _clock->init();

    _systemModule = std::make_unique<SystemModule>(*_clock);
    _graphicsModule = std::make_unique<GraphicsModule>(_options.graphics);
    _renderer = makeRenderer(
        _window->sdlWindow(),
        glm::ivec2 {_options.graphics.width, _options.graphics.height},
        _options.graphics.vsync,
        _options.vulkanValidation,
        _options.vulkanDebugLabels);
    _renderer->init();
    _graphicsModule->setRenderers(*_renderer, _renderer->renderer2d());
    imguiInit(*_renderer);
    _audioModule = std::make_unique<AudioModule>(_options.audio);
    _movieModule = std::make_unique<MovieModule>();
    _scriptModule = std::make_unique<ScriptModule>();
    _resourceModule = std::make_unique<ResourceModule>(
        gameId,
        _options.game.path,
        _options.graphics,
        _options.audio,
        *_graphicsModule,
        *_audioModule,
        *_scriptModule);
    _sceneModule = std::make_unique<SceneModule>(
        _options.graphics,
        *_resourceModule,
        *_graphicsModule,
        *_audioModule);
    _guiModule = std::make_unique<GUIModule>(
        _options.graphics,
        *_sceneModule,
        *_graphicsModule,
        *_resourceModule);
    _gameModule = std::make_unique<GameModule>(
        gameId,
        *_optionsView,
        *_resourceModule,
        *_graphicsModule,
        *_audioModule,
        *_sceneModule,
        *_scriptModule);
    _systemModule->init();
    _graphicsModule->init();
    _audioModule->init();
    _movieModule->init();
    _scriptModule->init();
    _resourceModule->init();
    _sceneModule->init();
    _guiModule->init();
    _gameModule->init();

    // The scene library cannot reach the renderer on its own; the engine owns
    // it and hands it over so the scene pipeline can be built.
    _sceneModule->renderPipelineFactory().setRenderer(*_renderer);

    _services = std::make_unique<ServicesView>(
        _gameModule->services(),
        _movieModule->services(),
        _audioModule->services(),
        _graphicsModule->services(),
        _sceneModule->services(),
        _guiModule->services(),
        _scriptModule->services(),
        _resourceModule->services(),
        _systemModule->services());

    _profiler = std::make_unique<Profiler>(
        _options.graphics,
        _services->graphics,
        _services->resource,
        _services->system);
    _profiler->init();
    _profiler->reserveThread(
        kMainThreadName,
        {glm::vec3 {0.0f, 1.0f, 1.0f},
         glm::vec3 {0.0f, 1.0f, 0.0f},
         glm::vec3 {1.0f, 0.0f, 0.0f},
         glm::vec3 {1.0f, 1.0f, 0.0f}});

    _console = std::make_unique<Console>(
        _options.graphics,
        _services->graphics,
        _services->resource);
    _console->init();
    _console->registerCommand("recompileshaders", "compile Slang shaders and rebuild render pipelines",
                              [this](const auto &) {
                                  const bool success = _renderer->recompileShaders();
                                  // Compute and ray-query pipelines own their shader modules, so
                                  // discard scene pipelines as well. They are rebuilt lazily before
                                  // the next draw after the device has gone idle above.
                                  _sceneModule->graphs().invalidateRenderPipelines();
                                  _console->printLine(success
                                                          ? "Slang shaders recompiled."
                                                          : "Slang errors kept one or more last-good shaders; see log.");
                              });
    // Seeded before any command can stage anything. Only the reapply fields of
    // this copy are ever read back.
    _stagedGraphics = _options.graphics;
    registerGraphicsCommands();
    _console->registerCommand("pause", "pause command-file execution for a number of rendered frames", [this](const auto &args) {
        auto frames = args.template get<int>(1);
        if (!frames || *frames < 1) {
            throw std::invalid_argument("usage: pause <frames>, where frames is positive");
        }
        _scriptPauseFrames = *frames;
    });
    _console->registerCommand("pausesec", "pause command-file execution for a number of seconds", [this](const auto &args) {
        auto seconds = args.template get<double>(1);
        if (!seconds || *seconds <= 0.0) {
            throw std::invalid_argument("usage: pausesec <seconds>, where seconds is positive");
        }
        _scriptPauseUntil = std::chrono::steady_clock::now() +
                            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(*seconds));
    });
    _console->registerCommand("capture", "capture one or more rendered frames", [this](const auto &args) {
        auto path = args[1];
        auto count = args.template get<int>(2).value_or(1);
        if (!path || count < 1) {
            throw std::invalid_argument("usage: capture <path> [frames], where frames is positive");
        }
        _captureRequest = CaptureRequest {std::filesystem::path(std::string(*path)), count, 0};
    });
    _console->registerCommand("quit", "end the engine run", [this](const auto &) {
        _scriptQuitRequested = true;
    });

    _game = std::make_unique<Game>(
        gameId,
        _options.game.path,
        *_optionsView,
        *_services,
        *_console);
    _game->init();
    // A long synchronous load draws a loading screen partway through. The game
    // cannot open a frame itself - the host owns frame boundaries - so it asks.
    _game->setPresentFrame([this]() {
        bool quit = false;
        renderFrame(quit);
    });

    _editor = std::make_unique<Editor>(*this, _options.game.developer);
    // The same capture the settings window's button takes, reachable from the
    // console so a scripted run can ask for one too - which is also the only
    // way to test it, since a button cannot be pressed headlessly.
    _console->registerCommand(
        "scenecapture", "write a full scene state capture beside the executable",
        [this](const game::ConsoleArgs &) { _editor->requestSceneCapture(); });
    if (!_options.inputScript.empty()) {
        // UI automation must not inherit a developer's persisted docking
        // layout: its client coordinates describe the fresh default layout.
        // Not nulled unconditionally any more. A hand-written click script is
        // written against ImGui's default layout, so it still gets one; a
        // recording carries the layout its coordinates were aimed at, and
        // throwing that away is what made a recorded settings change replay as
        // a session that clicked on nothing.
        ImGui::GetIO().IniFilename = nullptr;
        loadInputScript();
        if (!_replayImGuiIni.empty()) {
            ImGui::GetIO().IniFilename = _replayImGuiIni.c_str();
        }
    }
    openInputRecording();

    if (_options.commandsFrame == 0 || !_options.commandsFrameScheduledFile.empty()) {
        runCommandsFile(_options.commandsFile);
    }
}

void Engine::deinit() {
    _editor.reset();

    if (_renderer) {
        // Pipelines own images sampled by the last submitted command buffer;
        // release them only after that work has completed.
        _renderer->waitIdle();
    }

    // Before ImGui goes away. A render pipeline holds an ImGui descriptor set
    // for its target preview, allocated from a pool that ImGui's shutdown
    // destroys, so a pipeline outliving that shutdown releases a descriptor
    // against a pool that is already gone.
    _game.reset();
    _gameModule.reset();
    _guiModule.reset();
    _sceneModule.reset();

    imguiShutdown();

    _console.reset();
    _profiler.reset();
    _services.reset();

    _resourceModule.reset();
    _scriptModule.reset();
    _movieModule.reset();
    _audioModule.reset();
    _graphicsModule.reset();
    _systemModule.reset();
    _clock.reset();

    // The renderer holds the surface created from the window, so it has to go
    // before the window and before SDL_Quit. Left to its own destructor it
    // outlived both, since ~Engine runs after this function returns.
    if (_renderer) {
        _renderer->deinit();
        _renderer.reset();
    }

    _optionsView.reset();
    _window.reset();

    SDL_Quit();
}

int Engine::run() {
    auto &clock = _services->system.clock;
    // micros, to match the read below: seeding from millis made the first
    // frameTime the whole time since the clock started, which advanced every
    // animation in the scene by seconds before a single frame was drawn.
    _ticks = clock.micros();

    bool quit = false;
    while (!quit) {
        processEvents(quit);
        if (quit) {
            break;
        }
        // Idle while the window is in the background, but never during a
        // capture: the run is not being watched, it is being measured, and
        // stalling on focus makes the result depend on what else the desktop
        // was doing.
        if (!_window->isInFocus() && !_options.graphics.headless) {
            std::this_thread::sleep_for(std::chrono::milliseconds {100});
            continue;
        }
        uint64_t ticks = clock.micros();
        if (_game->consumeTimingDiscontinuity()) {
            // A synchronous load blocked somewhere in the last frame. Rebase
            // onto now so that interval is not charged to the world as elapsed
            // gameplay time: this frame opens a new epoch and starts at zero.
            _ticks = ticks;
        }
        auto frameTime = (ticks - _ticks) / 10e5f;
        _ticks = ticks;
        // Advanced before anything consumes the frame, so an event handled
        // this frame is stamped with the time the frame represents.
        _simClock += frameTime;
        if (_options.graphics.headless) {
            // A capture run exists to be compared against another one, which
            // only works if both see the same sequence of frames. Wall-clock
            // timing does not give that: the same frame number lands on
            // different animation state every run.
            //
            // The consequence is that such a run is not played at real speed:
            // the simulation advances a sixtieth of a second per frame however
            // long the frame took, so it appears fast on a light scene and slow
            // on a heavy one. That is the point, and it does not affect what is
            // captured.
            frameTime = 1.0f / 60.0f;
        }
        _profiler->measure(kMainThreadName, kProfilerInputTimeIndex, [this, &quit]() {
            R_PROFILE_ZONE("input");
            while (!_events.empty()) {
                auto event = _events.front();
                _events.pop();
                if (_profiler->handle(event)) {
                    continue;
                }
                if (_console->handle(event)) {
                    continue;
                }
                if (_game->handle(event)) {
                    if (_game->isQuitRequested()) {
                        quit = true;
                        break;
                    }
                    continue;
                }
            }
        });
        if (quit) {
            break;
        }
        ++_frameIndex;
        if (_options.commandsFrame > 0 && _frameIndex >= _options.commandsFrame &&
            !_commandsRun) {
            _commandsRun = true;
            runCommandsFile(_options.commandsFrameScheduledFile.empty()
                                ? _options.commandsFile
                                : _options.commandsFrameScheduledFile);
        }
        processScriptedCommands(quit);
        if (quit) {
            break;
        }
        // Ahead of the update slot, because the slot opens the ImGui frame and
        // the editor's render-target viewer submits an ImGui image handle owned
        // by the scene pipeline. Rebuilding after that point would free the
        // handle out from under draw data that has already been recorded, and
        // the frame would be presented with a descriptor that no longer exists.
        // A rebuild asked for during this frame's input or commands file is
        // therefore taken now, before anything can reference the old pipeline.
        applyGraphicsRebuild();
        _profiler->measure(kMainThreadName, kProfilerUpdateTimeIndex, [this, &frameTime]() {
            R_PROFILE_ZONE("update");
            imguiBeginFrame();
            _game->update(frameTime);
            bool showcur = _game->cursorType() == CursorType::None;
            bool relmouse = _game->relativeMouseMode();
            if (_editor && _editor->isEnabled()) {
                // The in-game camera grabs the pointer, which would make editor
                // windows unreachable. Release it for as long as the editor is up.
                // Cursor visibility is left to ImGui, which drives it every frame
                // from the cursor imguiBeginFrame selects.
                relmouse = false;
            }
            showCursor(showcur);
            setRelativeMouseMode(relmouse);
            _profiler->update(frameTime);
            if (_editor) {
                _editor->update(frameTime);
            }
        });
        _profiler->measure(kMainThreadName, kProfilerRenderGraphicsTimeIndex, [this, &quit]() {
            R_PROFILE_ZONE("graphics");
            renderFrame(quit);
        });
        _profiler->measure(kMainThreadName, kProfilerRenderAudioTimeIndex, [this]() {
            R_PROFILE_ZONE("audio");
            _services->audio.mixer.render();
        });
        // A module/save load presents loading-screen frames. Defer it until
        // the regular ImGui frame has been rendered and closed.
        if (_editor) {
            _editor->applyPendingTransition();
        }
    }

    return 0;
}

/**
 * The RenderDoc in-application API, when the process was launched under
 * RenderDoc. renderdoccmd has no option to capture a particular frame, and
 * triggering by keypress does not suit an unattended run, so the frame we
 * screenshot is the frame we ask RenderDoc for.
 */
static RENDERDOC_API_1_1_2 *renderdocApi() {
#ifdef _WIN32
    static RENDERDOC_API_1_1_2 *api = []() -> RENDERDOC_API_1_1_2 * {
        auto module = GetModuleHandleA("renderdoc.dll");
        if (!module) {
            return nullptr;
        }
        auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
        if (!getApi) {
            return nullptr;
        }
        RENDERDOC_API_1_1_2 *result = nullptr;
        if (getApi(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void **>(&result)) != 1) {
            return nullptr;
        }
        return result;
    }();
    return api;
#else
    return nullptr;
#endif
}

void Engine::captureIfRequested(bool &quit) {
    if (!_captureRequest) {
        return;
    }
    if (_options.renderdoc && !_renderdocTriggered) {
        if (auto api = renderdocApi()) {
            api->TriggerCapture();
            info("RenderDoc capture triggered");
        } else {
            warn("--renderdoc given but the process is not running under RenderDoc");
        }
        _renderdocTriggered = true;
    }
    captureFrame(numberedCapturePath(*_captureRequest));
    ++_captureRequest->index;
    if (_captureRequest->index >= _captureRequest->count) {
        _captureRequest.reset();
        dumpTargetsIfRequested();
        dumpObjectsIfRequested();
    }
}

void Engine::captureFrame(const std::filesystem::path &path) {
    auto screenshot = _services->graphics.renderer.captureFrame();
    auto stream = FileOutputStream(path);
    TgaWriter(screenshot).save(stream);
    // What this frame is a frame OF, logged beside it. Two captures may only
    // be compared when these lines match: a conversation that advanced the
    // camera in one run and not the other produced images of two different
    // moments, and nothing in the images said so.
    info("CAPTURESTATE " + path.filename().string() + " " + _game->captureStateDigest());
    info("Wrote screenshot: " + path.string());
}

std::filesystem::path Engine::numberedCapturePath(const CaptureRequest &request) const {
    if (request.count == 1) {
        return request.path;
    }
    std::ostringstream stem;
    stem << request.path.stem().string() << '-' << std::setfill('0') << std::setw(4) << (request.index + 1);
    return request.path.parent_path() / (stem.str() + request.path.extension().string());
}

/**
 * The raw material for the curation pass: every object the tracer still
 * classifies emissive by default, appended as one tab-separated line so a warp
 * loop over the module list accumulates the game-wide candidate set in one
 * file. Danglies are out (stripped by default already), doors are out (stripped
 * by admission for being doors - see isDoorMesh), the sky room is out, and
 * anything already curated is out - what remains is exactly the set a
 * name-based classifier has to rule on.
 */
void Engine::dumpObjectsIfRequested() {
    if (_options.dumpObjectsPath.empty()) {
        return;
    }
    auto &graph = _services->scene.graphs.get(kSceneMain);
    auto &scene = graph.gpuScene();
    const auto &materials = scene.traceMaterials();
    auto module = _game->module();
    std::string moduleName = module ? module->name() : "?";
    std::set<std::string> lines;
    for (const auto &object : scene.objects()) {
        const auto *mesh = std::get_if<scene::RegisteredMesh>(&object);
        if (!mesh ||
            !glm::any(glm::greaterThan(mesh->material.selfIllumColor, glm::vec3(0.0f))) ||
            std::holds_alternative<scene::RegisteredDangly>(mesh->deformation) ||
            scene::isDoorMesh(*mesh) ||
            (mesh->cullRoot && mesh->cullRoot == scene.skyRoom()) ||
            materials.curatedByIndex(mesh->material.curatedIndex)) {
            continue;
        }
        std::string model {graph.nameText(mesh->nameIds.model)};
        std::string node {graph.nameText(mesh->nameIds.node)};
        const auto *diffuse =
            mesh->material.textures[static_cast<size_t>(graphics::MaterialTextureSlot::MainTex)];
        const auto &illum = mesh->material.selfIllumColor;
        std::ostringstream line;
        line << moduleName << "\t" << model << "/" << node << "\t"
             << (diffuse ? diffuse->name() : "-") << "\t"
             << illum.r << " " << illum.g << " " << illum.b;
        lines.insert(line.str());
    }
    std::ofstream out(_options.dumpObjectsPath, std::ios::app);
    for (const auto &line : lines) {
        out << line << "\n";
    }
    // The completion header, written last: its presence proves this module's
    // block is whole (a killed run leaves lines but no header), and the
    // admitted object count is the evidence the module actually loaded -
    // "0 candidates" from a real scene holds hundreds of objects, from a
    // failed warp near none.
    out << "# " << moduleName << " objects=" << scene.objects().size()
        << " candidates=" << lines.size() << "\n";
    info("Dumped " + std::to_string(lines.size()) + " emissive candidates of " + moduleName +
         " to " + _options.dumpObjectsPath);
}

void Engine::dumpTargetsIfRequested() {
    if (_options.dumpTargetsPath.empty()) {
        return;
    }
    // Every scene that has rendered, not just "main". The main menu draws into
    // the "mainmenu" graph, so asking only for kSceneMain made --dumptargets
    // silently produce nothing there - which is exactly the scene whose smoke
    // is under investigation. Each scene gets its own subdirectory so a
    // multi-scene frame does not overwrite itself.
    std::vector<std::pair<std::string, IRenderPipeline *>> rendered;
    for (const auto &name : _services->scene.graphs.sceneNames()) {
        if (auto *pipeline = _services->scene.graphs.get(name).renderPipeline()) {
            rendered.push_back({name, pipeline});
        }
    }
    if (rendered.empty()) {
        warn("--dumptargets given but no scene has been rendered");
        return;
    }
    // The frame this describes has to be finished before its targets are read.
    // Vulkan needs its recorded commands submitted before the targets are read.
    _renderer->flushFrame();
    for (const auto &[name, pipeline] : rendered) {
        std::filesystem::path dir = _options.dumpTargetsPath;
        if (rendered.size() > 1) {
            dir /= name;
        }
        std::filesystem::create_directories(dir);
        pipeline->dumpTargets(dir);
        info("Dumped targets for scene '" + name + "' to " + dir.string());
    }
}

void Engine::renderFrame(bool &quit) {
    if (_inFrame) {
        // Reached from inside a frame. The game asks for one while a module
        // loads, and that request must not arrive mid-frame; if it ever does,
        // dropping it is safer than nesting frame boundaries.
        return;
    }
    _inFrame = true;
    _services->graphics.statistic.resetDrawCalls();
    renderVulkanFrame(quit);
    _inFrame = false;
}

void Engine::renderVulkanFrame(bool &quit) {
    glm::ivec2 extent {_options.graphics.width, _options.graphics.height};
    // Before the frame's rendering scope: the scene pipeline begins render
    // passes of its own, and one cannot be nested inside another.
    _renderer->beginFrame(extent);
    imguiBeginFrame();
    _game->renderSceneOffscreen();

    // Keep scene composite and console in one 2D batch. The renderer owns the
    // dynamic-rendering scope and its physical swapchain extent.
    {
        R_PROFILE_ZONE("Renderer::2D record");
        _renderer->with2DRendering(extent, [this]() {
            _game->render();
            _console->render();
        });
    }

    imguiRender();
    captureIfRequested(quit);
    // Here for the same reason the screenshot above is: the finished frame is
    // still readable, and the renderer will accept a flush. A capture asked for
    // from the console or a button arrives outside any frame, where both fail.
    if (_editor) {
        _editor->performPendingSceneCapture();
    }
    _renderer->endFrame();
    R_PROFILE_FRAME_MARK();
}

void Engine::applyGraphicsRebuild() {
    // Asked first, unconditionally, so the flag is cleared even on the frames a
    // graphics Apply is also pending: leaving it set would rebuild once more on
    // the following frame for nothing.
    const bool sceneAsked = _sceneModule->graphs().consumeRenderPipelineRebuild();
    if (!_graphicsRebuildRequested && !sceneAsked) {
        return;
    }
    _graphicsRebuildRequested = false;
    // The staged values become the live ones here and nowhere else, so no frame
    // ever renders with options its pipeline was not built for.
    if (_graphicsCommitPending) {
        commitStagedGraphics();
    }
    // The window and the swapchain first; setVsync flags the swapchain for
    // recreation, which the next beginFrame acts on with the new extent.
    _window->resize(_options.graphics.width, _options.graphics.height);
    _renderer->setVsync(_options.graphics.vsync);
    // Everything below releases device memory, image views and descriptor sets
    // that frames still in flight may be reading, and this now runs outside a
    // frame, so nothing has waited for them. The blunt wait is the correct one:
    // a rebuild is a rare, deliberate act, and it must not depend on which
    // other option happened to change alongside.
    _renderer->waitIdle();
    // Destroying a scene's pipeline destroys its scene pipeline, its ray-query
    // pipeline if it had one, the sky bake, the device-side GpuScene and the
    // admission layer. The next SceneGraph::render rebuilds all of it, reading
    // the render mode afresh, so this is also what makes a mode switch happen.
    _sceneModule->graphs().invalidateRenderPipelines();
}

std::vector<std::string> Engine::stagedGraphicsChanges() const {
    return graphics::graphicsOptionsDiffering(_stagedGraphics, _options.graphics,
                                              graphics::OptionApply::Reapply);
}

void Engine::applyStagedGraphics() {
    if (stagedGraphicsChanges().empty()) {
        return;
    }
    // Asked for, not performed. The copy happens in applyGraphicsRebuild,
    // immediately before the pipelines are thrown away.
    //
    // Copying here instead put the two an unbounded distance apart, because the
    // rebuild is a flag consumed at the TOP of a frame while this is called
    // from wherever the request came from. From the settings window that is the
    // middle of one: the live options became PBR, the rest of that frame
    // rendered on a pipeline still built for retro, and pbrChannelsPass barrier
    // -ed channel images retro never allocates - a null dereference in
    // VulkanCommandBuffer::imageBarrier. The console never showed it because
    // its commands are processed on the line above applyGraphicsRebuild, so its
    // copy and rebuild are always back to back.
    //
    // Deferring costs one frame rendered with the old options, which is exactly
    // right: those are the options the pipeline in front of it was built for.
    _graphicsCommitPending = true;
    requestGraphicsRebuild();
}

void Engine::commitStagedGraphics() {
    _graphicsCommitPending = false;
    auto changed = stagedGraphicsChanges();
    if (changed.empty()) {
        return;
    }
    graphics::copyGraphicsOptions(_stagedGraphics, _options.graphics,
                                  graphics::OptionApply::Reapply);
    std::string message = "Graphics options reapplied:";
    for (const auto &name : changed) {
        message += " " + name + "=" +
                   graphics::findGraphicsOptionDesc(name)->get(_options.graphics) + ";";
    }
    info(message, LogChannel::Graphics);
}

void Engine::revertStagedGraphics() {
    graphics::copyGraphicsOptions(_options.graphics, _stagedGraphics,
                                  graphics::OptionApply::Reapply);
}

std::string Engine::setGraphicsOption(const std::string &name, const std::string &value) {
    const auto *desc = graphics::findGraphicsOptionDesc(name);
    if (!desc) {
        throw std::invalid_argument("Unknown graphics option '" + name +
                                    "'; 'gfx list' names them all");
    }
    // Classified against the value being set, not against the option: the
    // render mode is live between the two raster resolves and a rebuild only
    // when it crosses into or out of path tracing. Parsing into a candidate
    // first also means an unreadable value throws before anything is written.
    graphics::GraphicsOptions candidate = _options.graphics;
    desc->set(candidate, value);
    switch (graphics::graphicsOptionApply(*desc, _options.graphics, candidate)) {
    case graphics::OptionApply::Reapply: {
        // Into the staged copy only. Writing the live struct here would leave
        // the running frame describing targets that were never allocated.
        desc->set(_stagedGraphics, value);
        std::string report;
        if (desc->equal(_stagedGraphics, _options.graphics)) {
            report = name + " = " + desc->get(_stagedGraphics) +
                     " (unchanged; requires reapply)";
        } else {
            report = name + " = " + desc->get(_stagedGraphics) + " staged, was " +
                     desc->get(_options.graphics) + "; run 'gfx apply' to rebuild";
        }
        if (name == "mode") {
            // The command line resolves the anti-aliasing default from the
            // render mode, in optionsparser.cpp, because it is the only place
            // that sees both before either is used. A runtime switch cannot:
            // the slot already holds whatever it was given. Say what it holds,
            // or a mode reached through the console quietly differs from the
            // same mode given as --mode at startup.
            const auto *slot = graphics::findGraphicsOptionDesc("antialiasing");
            report += "; the anti-aliasing slot stays '" +
                      slot->get(_stagedGraphics) + "' (--mode would default it to " +
                      (desc->get(_stagedGraphics) == "path-tracing" ? "fsr" : "fxaa") + ")";
        }
        return report;
    }
    case graphics::OptionApply::Restart:
        // Recorded, and said so. The consumer ran before anything that could be
        // rebuilt existed, so pretending otherwise would be the silent failure.
        desc->set(_options.graphics, value);
        desc->set(_stagedGraphics, value);
        return name + " = " + desc->get(_options.graphics) +
               " recorded, but it is only read at startup; this run is unaffected";
    default:
        desc->set(_options.graphics, value);
        desc->set(_stagedGraphics, value);
        return name + " = " + desc->get(_options.graphics) + " (live, from the next frame)";
    }
}

void Engine::registerGraphicsCommands() {
    _console->registerCommand(
        "gfx",
        "graphics options: gfx set <option> <value> | gfx apply | gfx revert | "
        "gfx get <option> | gfx list [substring]",
        [this](const game::ConsoleArgs &args) {
            // A commands file is the only way a capture run can be scripted, so
            // every failure here has to reach the log as well as the console -
            // a headless run has nobody watching the console, and a typo that
            // printed nowhere would leave the capture silently describing the
            // options the run started with.
            const auto fail = [this](const std::string &message) {
                error("gfx: " + message, LogChannel::Graphics);
                _console->printLine("gfx: " + message);
            };
            const auto say = [this](const std::string &message) {
                info("gfx: " + message, LogChannel::Graphics);
                _console->printLine("gfx: " + message);
            };
            const auto token = [&args](size_t index) {
                return std::string(args[index].value_or(std::string_view {}));
            };
            const auto subcommand = token(1);
            if (subcommand.empty()) {
                fail("expected a subcommand: set, apply, revert, get or list");
                return;
            }
            if (subcommand == "set") {
                if (args.size() < 4) {
                    fail("set expects an option name and a value");
                    return;
                }
                // Values carry no spaces today, but joining the tail keeps a
                // trailing comment or a stray separator in a commands file from
                // being dropped without a word.
                std::string value = token(3);
                for (size_t i = 4; i < args.size(); ++i) {
                    value += " " + token(i);
                }
                try {
                    say(setGraphicsOption(token(2), value));
                } catch (const std::exception &ex) {
                    fail(ex.what());
                }
                return;
            }
            if (subcommand == "apply") {
                auto changed = stagedGraphicsChanges();
                if (changed.empty()) {
                    say("nothing staged");
                    return;
                }
                applyStagedGraphics();
                std::string message = "reapplying";
                for (const auto &name : changed) {
                    message += " " + name;
                }
                say(message);
                return;
            }
            if (subcommand == "revert") {
                const auto count = stagedGraphicsChanges().size();
                revertStagedGraphics();
                say(count == 0 ? "nothing staged"
                               : "discarded " + std::to_string(count) + " staged change(s)");
                return;
            }
            if (subcommand == "get") {
                const auto name = token(2);
                if (name.empty()) {
                    fail("get expects an option name");
                    return;
                }
                const auto *desc = graphics::findGraphicsOptionDesc(name);
                if (!desc) {
                    fail("unknown graphics option '" + name + "'; 'gfx list' names them all");
                    return;
                }
                // The class of a change FROM the current value, which for a
                // value-dependent option is not the same as the option's own.
                std::string line = desc->name + " = " + desc->get(_options.graphics) +
                                   " [" +
                                   graphics::optionApplyName(graphics::graphicsOptionApply(
                                       *desc, _options.graphics, _stagedGraphics)) +
                                   "]";
                if (!desc->equal(_stagedGraphics, _options.graphics)) {
                    line += ", staged " + desc->get(_stagedGraphics);
                }
                say(line);
                return;
            }
            if (subcommand == "list") {
                const auto filter = token(2);
                int shown = 0;
                for (const auto &desc : graphics::graphicsOptionDescs()) {
                    if (!filter.empty() && desc.name.find(filter) == std::string::npos) {
                        continue;
                    }
                    ++shown;
                    std::string line = desc.name + " = " + desc.get(_options.graphics) +
                                       " [" +
                                       graphics::optionApplyName(graphics::graphicsOptionApply(
                                           desc, _options.graphics, _stagedGraphics)) +
                                       "] " + desc.help;
                    if (!desc.equal(_stagedGraphics, _options.graphics)) {
                        line += " (staged " + desc.get(_stagedGraphics) + ")";
                    }
                    say(line);
                }
                if (shown == 0) {
                    fail("no graphics option matches '" + filter + "'");
                }
                return;
            }
            fail("unknown subcommand '" + subcommand +
                 "'; expected set, apply, revert, get or list");
        });
}

void Engine::runCommandsFile(const std::string &path) {
    if (path.empty()) {
        return;
    }
    std::ifstream file(path);
    if (!file.good()) {
        throw std::runtime_error("Failed to open commands file: " + path);
    }
    for (std::string line; std::getline(file, line);) {
        // getline keeps the carriage return of a CRLF file, which would end up
        // inside the last argument of every command.
        //
        // Remove trailing and leading spaces as well.

        std::string_view command = string_strip(line);
        if (!command.empty()) {
            _scriptedCommands.emplace_back(command);
        }
    }
}

void Engine::processScriptedCommands(bool &quit) {
    if (_scriptQuitRequested) {
        quit = true;
        return;
    }
    if (_captureRequest) {
        return;
    }
    if (_scriptPauseFrames > 0) {
        --_scriptPauseFrames;
        if (_scriptPauseFrames > 0) {
            return;
        }
    }
    if (_scriptPauseUntil) {
        // Checked per frame rather than slept through: the loop has to keep
        // rendering and pumping events for the wait to mean anything, and a
        // sleep here would stall the window instead of letting the game run.
        if (std::chrono::steady_clock::now() < *_scriptPauseUntil) {
            return;
        }
        _scriptPauseUntil.reset();
    }
    while (!_scriptedCommands.empty()) {
        std::string command(std::move(_scriptedCommands.front()));
        _scriptedCommands.pop_front();
        _console->execute(command);
        if (_scriptQuitRequested) {
            quit = true;
            return;
        }
        if (_captureRequest || _scriptPauseFrames > 0 || _scriptPauseUntil) {
            return;
        }
    }
}

void Engine::processEvents(bool &quit) {
    std::queue<input::Event> unhandled;
    auto processEvent = [this, &quit, &unhandled](SDL_Event &sdlEvent, bool automated) {
        // First, ahead of every return below. Two of them fire before the game
        // ever sees an event - one for an event belonging to another window, one
        // for a window event the window itself consumes - and ImGui's viewport
        // windows arrive through exactly the first of those. Recording after
        // them captured the world clicks and silently dropped every click on an
        // ImGui window, which is most of what there is to record.
        if (!automated) {
            recordInputEvent(sdlEvent);
        } else if (sdlEvent.type == SDL_EVENT_MOUSE_MOTION) {
            // Remembered, never warped. ImGui's SDL3 backend re-reads the real
            // cursor in NewFrame whenever the window has focus and would
            // otherwise overwrite this, but the answer is to write the position
            // into ImGui after NewFrame - not to move the developer's pointer.
            // Taking the physical mouse for the length of a replay makes the
            // machine unusable and is never acceptable.
            g_replayMousePos = ImVec2 {sdlEvent.motion.x, sdlEvent.motion.y};
        }
        if (sdlEvent.type == SDL_EVENT_QUIT) {
            quit = true;
            return;
        }
        if (!_window->isAssociatedWith(sdlEvent)) {
            imguiHandle(sdlEvent);
            return;
        }
        if (_window->handle(sdlEvent)) {
            if (_window->isCloseRequested()) {
                quit = true;
            }
            return;
        }
        auto event = eventFromSDLEvent(sdlEvent);
        if (!event) {
            return;
        }
        if (!automated && !_automatedInput.empty() && !_replayFinished) {
            // A replay owns the input. A stray real mouse move or click while
            // one is running changes what the session does from that point on,
            // so the replay stops being a replay - which is exactly what makes
            // a divergence impossible to tell from a genuine difference in the
            // thing being measured. Real input resumes once the script is spent.
            return;
        }
        if (_options.graphics.headless && !automated) {
            // Dropped rather than handled. A single mouse move over the window
            // turns the camera, and from then on frame 900 is a different
            // frame - which is most of why two runs of the same build did not
            // match. Console commands still arrive, through the commands file
            // rather than through here.
            return;
        }
        if (_profiler->handle(*event)) {
            return;
        }
        if (_editor && _editor->handle(*event)) {
            return;
        }
        // Last filter before the game sees it: ImGui only claims the event when
        // it actually wants the mouse or keyboard.
        if (imguiHandle(sdlEvent)) {
            return;
        }
        unhandled.push(*event);
    };

    SDL_Event sdlEvent;
    while (SDL_PollEvent(&sdlEvent)) {
        processEvent(sdlEvent, false);
        if (quit) {
            break;
        }
    }
    while (!quit && _nextAutomatedInput < _automatedInput.size()) {
        const auto &next = _automatedInput[_nextAutomatedInput];
        // Two clocks, because two kinds of entry. A recorded session is
        // stamped in simulated seconds and is due when the clock passes it;
        // a hand-written capture script is frame-indexed and is due on its
        // frame. Both live in one queue, sorted by whichever they carry.
        const bool due = next.frame >= 0 ? next.frame <= _frameIndex + 1
                                         : next.time <= _simClock;
        if (!due) {
            break;
        }
        auto event = _automatedInput[_nextAutomatedInput++].event;
        processEvent(event, true);
    }
    // A replay ends when the recording does. Sitting on the last frame waiting
    // for a pausesec that was guessed at is not an ending - it leaves the window
    // up after there is nothing left to do, and makes the run's length a number
    // someone had to pick instead of a property of the recording.
    if (!_automatedInput.empty() && _nextAutomatedInput >= _automatedInput.size() &&
        !_replayFinished) {
        const auto &last = _automatedInput.back();
        const float endsAt = last.frame >= 0 ? 0.0f : last.time;
        // A short settle after the final event, so whatever it started - a load,
        // a mode change - is on screen and in the profile before the run ends.
        if (last.frame >= 0 ? _frameIndex >= last.frame + 120 : _simClock >= endsAt + 2.0f) {
            _replayFinished = true;
            info("Input script finished; closing", LogChannel::Global);
            quit = true;
        }
    }
    while (!unhandled.empty()) {
        _events.push(std::move(unhandled.front()));
        unhandled.pop();
    }
}

void Engine::openInputRecording() {
    if (_options.recordInput.empty()) {
        return;
    }
    _inputRecording.open(_options.recordInput, std::ios::out | std::ios::trunc);
    if (!_inputRecording) {
        throw std::runtime_error("Failed to open input recording: " + _options.recordInput);
    }
    _inputRecording << "# reone-input 1\n";
    // The ImGui layout goes into the recording, because the coordinates do not
    // mean anything without it. A click at 1901,712 hits Apply in the layout the
    // developer had docked; in ImGui's default layout it hits whatever happens
    // to be there, which is how a recording of a settings change replayed as a
    // session that never changed a setting.
    _inputRecording << "@mainwindow " << SDL_GetWindowID(_window->sdlWindow()) << "\n";
    if (const char *ini = ImGui::GetIO().IniFilename) {
        std::ifstream layout(ini);
        for (std::string line; std::getline(layout, line);) {
            _inputRecording << "@ini " << line << "\n";
        }
    }
    if (!_options.commandsFile.empty()) {
        _inputRecording << "# started with --commands-file " << _options.commandsFile << "\n";
    }
}

void Engine::recordInputEvent(const SDL_Event &event) {
    if (!_inputRecording) {
        return;
    }
    // Flushed per event at the end of this function. A few hundred events over a
    // session costs nothing, and a buffered stream loses whatever it still holds
    // if the process is killed rather than closed - which is exactly the tail of
    // the recording, where whatever went wrong was happening.
    struct Flush {
        std::ofstream &out;
        ~Flush() { out.flush(); }
    } flush {_inputRecording};
    // Only what the replay can reconstruct. An event written but not parseable
    // would shift the meaning of every entry after it, which is worse than
    // dropping it.
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        _inputRecording << _simClock << " motion " << static_cast<int>(event.motion.x) << " "
                        << static_cast<int>(event.motion.y) << " " << event.motion.windowID
                        << "\n";
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        _inputRecording << _simClock << " button "
                        << (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down" : "up") << " "
                        << static_cast<int>(event.button.button) << " "
                        << static_cast<int>(event.button.x) << " "
                        << static_cast<int>(event.button.y) << " " << event.button.windowID
                        << "\n";
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        _inputRecording << _simClock << " wheel " << event.wheel.x << " " << event.wheel.y << " "
                        << event.wheel.windowID << "\n";
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        // Repeats are dropped: the replay regenerates them from the held state,
        // and writing them makes a file that is mostly repeat.
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat) {
            break;
        }
        _inputRecording << _simClock << " key "
                        << (event.type == SDL_EVENT_KEY_DOWN ? "down" : "up") << " "
                        << static_cast<int>(event.key.scancode) << " "
                        << static_cast<uint32_t>(event.key.key) << " "
                        << static_cast<uint32_t>(event.key.mod) << " " << event.key.windowID
                        << "\n";
        break;
    default:
        break;
    }
}

void Engine::loadInputScript() {
    std::ifstream file(_options.inputScript);
    if (!file.good()) {
        throw std::runtime_error("Failed to open input script: " + _options.inputScript);
    }

    auto addClick = [this](int frame, int x, int y) {
        auto windowId = SDL_GetWindowID(_window->sdlWindow());
        SDL_Event motion {};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.windowID = windowId;
        motion.motion.x = static_cast<float>(x);
        motion.motion.y = static_cast<float>(y);
        _automatedInput.push_back({frame, 0.0f, motion});

        SDL_Event down {};
        down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        down.button.windowID = windowId;
        down.button.button = SDL_BUTTON_LEFT;
        down.button.down = true;
        down.button.clicks = 1;
        down.button.x = static_cast<float>(x);
        down.button.y = static_cast<float>(y);
        _automatedInput.push_back({frame + 1, 0.0f, down});

        SDL_Event up = down;
        up.type = SDL_EVENT_MOUSE_BUTTON_UP;
        up.button.down = false;
        _automatedInput.push_back({frame + 2, 0.0f, up});
    };

    const auto windowId = SDL_GetWindowID(_window->sdlWindow());
    std::vector<std::string> imguiLayout;
    // The id the main window had when this was recorded. Every other id in the
    // file belongs to an ImGui viewport window and is replayed unchanged; the
    // main one is remapped, because SDL will not hand out the same number twice.
    uint32_t recordedMainWindow = 0;
    const auto replayWindow = [&](uint32_t recorded) {
        return recorded == 0 || recorded == recordedMainWindow ? windowId : recorded;
    };
    bool sawFrameEntry = false;
    bool sawTimeEntry = false;

    for (std::string line; std::getline(file, line);) {
        std::string_view stripped = string_strip(line);
        if (stripped.empty() || stripped[0] == '#') {
            continue;
        }
        if (stripped[0] == '@') {
            // A console command the recording carries - which save to load, what
            // to seed. Queued rather than run here: nothing exists to run it
            // against yet, and the commands-file path already knows how to.
            //
            // The directive word goes too, not just the '@'. Leaving it made the
            // console receive "command loadgame 345" and reject it as an unknown
            // command, silently - the replay then ran the whole recording against
            // the main menu.
            // Not stripped on the right: an ImGui layout has blank lines
            // between its sections, and "@ini" with nothing after it is one of
            // them. Requiring an argument threw the whole replay away over a
            // blank line.
            std::string_view directive = stripped.substr(1);
            while (!directive.empty() && directive.front() == ' ') {
                directive.remove_prefix(1);
            }
            const auto space = directive.find(' ');
            const std::string_view name =
                space == std::string_view::npos ? directive : directive.substr(0, space);
            const std::string_view payload =
                space == std::string_view::npos ? std::string_view {} : directive.substr(space + 1);
            if (name != "command" && name != "ini" && name != "mainwindow") {
                throw std::runtime_error("Unknown input script directive '" + std::string {name} +
                                         "': " + line);
            }
            if (name != "ini" && payload.empty()) {
                throw std::runtime_error("Input script directive takes an argument: " + line);
            }
            if (name == "mainwindow") {
                recordedMainWindow = static_cast<uint32_t>(std::stoul(std::string {string_strip(payload)}));
                continue;
            }
            if (name == "ini") {
                imguiLayout.emplace_back(payload);
                continue;
            }
            _scriptedCommands.emplace_back(string_strip(payload));
            continue;
        }
        std::istringstream stream {std::string {stripped}};
        std::string stamp, action;
        if (!(stream >> stamp >> action)) {
            throw std::runtime_error("Invalid input script line: " + line);
        }
        if (action == "click") {
            // The original grammar, frame-indexed, still parsed: every capture
            // script in the diagnostics recipes is written in it.
            const int frame = std::stoi(stamp);
            int x, y;
            if (!(stream >> x >> y) || frame < 1) {
                throw std::runtime_error("Invalid input script line: " + line);
            }
            sawFrameEntry = true;
            addClick(frame, x, y);
            continue;
        }
        sawTimeEntry = true;
        const float when = std::stof(stamp);
        SDL_Event event {};
        if (action == "motion") {
            int x, y;
            if (!(stream >> x >> y)) {
                throw std::runtime_error("Invalid input script line: " + line);
            }
            uint32_t recorded = recordedMainWindow;
            stream >> recorded;
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.windowID = replayWindow(recorded);
            event.motion.x = static_cast<float>(x);
            event.motion.y = static_cast<float>(y);
        } else if (action == "button") {
            std::string dir;
            int button, x, y;
            if (!(stream >> dir >> button >> x >> y)) {
                throw std::runtime_error("Invalid input script line: " + line);
            }
            const bool down = dir == "down";
            event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            uint32_t recorded = recordedMainWindow;
            stream >> recorded;
            event.button.windowID = replayWindow(recorded);
            event.button.button = static_cast<uint8_t>(button);
            event.button.down = down;
            event.button.clicks = 1;
            event.button.x = static_cast<float>(x);
            event.button.y = static_cast<float>(y);
        } else if (action == "wheel") {
            float x, y;
            if (!(stream >> x >> y)) {
                throw std::runtime_error("Invalid input script line: " + line);
            }
            uint32_t recorded = recordedMainWindow;
            stream >> recorded;
            event.type = SDL_EVENT_MOUSE_WHEEL;
            event.wheel.windowID = replayWindow(recorded);
            event.wheel.x = x;
            event.wheel.y = y;
        } else if (action == "key") {
            std::string dir;
            uint32_t scancode, key, mod;
            if (!(stream >> dir >> scancode >> key >> mod)) {
                throw std::runtime_error("Invalid input script line: " + line);
            }
            const bool down = dir == "down";
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            uint32_t recorded = recordedMainWindow;
            stream >> recorded;
            event.key.windowID = replayWindow(recorded);
            event.key.scancode = static_cast<SDL_Scancode>(scancode);
            event.key.key = static_cast<SDL_Keycode>(key);
            event.key.mod = static_cast<SDL_Keymod>(mod);
            event.key.down = down;
        } else {
            throw std::runtime_error("Invalid input script line: " + line);
        }
        _automatedInput.push_back({-1, when, event});
    }

    if (!imguiLayout.empty()) {
        // Written beside the script so the replay is self-contained and does not
        // disturb whatever layout the developer has docked in build/bin.
        _replayImGuiIni = _options.inputScript + ".imgui.ini";
        std::ofstream ini(_replayImGuiIni, std::ios::out | std::ios::trunc);
        for (const auto &line : imguiLayout) {
            ini << line << "\n";
        }
    }
    if (sawFrameEntry && sawTimeEntry) {
        // Refused rather than guessed at. The two stamps are different clocks -
        // one counts frames, the other simulated seconds - and interleaving them
        // needs a conversion that is only correct headless, where a frame is
        // exactly 1/60s. A file that mixes them is a mistake, not a request.
        throw std::runtime_error(
            "Input script mixes frame-indexed 'click' entries with timed ones: " +
            _options.inputScript);
    }
    std::stable_sort(_automatedInput.begin(), _automatedInput.end(),
                     [](const auto &lhs, const auto &rhs) {
                         return lhs.frame >= 0 ? lhs.frame < rhs.frame : lhs.time < rhs.time;
                     });
}

void Engine::showCursor(bool show) {
    if (_showCursor == show) {
        return;
    }
    if (show) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
    _showCursor = show;
}

void Engine::setRelativeMouseMode(bool relative) {
    if (_relativeMouseMode == relative) {
        return;
    }
    _window->setRelativeMouseMode(relative);
    _relativeMouseMode = relative;
}

static constexpr int scaleWinCoord(int coord, int winScale) {
    return coord * 100 / winScale;
}

std::optional<input::Event> Engine::eventFromSDLEvent(const SDL_Event &sdlEvent) const {
    switch (sdlEvent.type) {
    case SDL_EVENT_KEY_DOWN:
        return input::Event::newKeyDown(input::KeyEvent {
            sdlEvent.key.down,
            static_cast<input::KeyCode>(sdlEvent.key.key),
            sdlEvent.key.mod,
            sdlEvent.key.repeat});
    case SDL_EVENT_KEY_UP:
        return input::Event::newKeyUp(input::KeyEvent {
            sdlEvent.key.down,
            static_cast<input::KeyCode>(sdlEvent.key.key),
            sdlEvent.key.mod,
            sdlEvent.key.repeat});
    case SDL_EVENT_MOUSE_MOTION:
        return input::Event::newMouseMotion(input::MouseMotionEvent {
            scaleWinCoord(sdlEvent.motion.x, _options.graphics.winScale),
            scaleWinCoord(sdlEvent.motion.y, _options.graphics.winScale),
            sdlEvent.motion.xrel,
            sdlEvent.motion.yrel});
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return input::Event::newMouseButtonDown(input::MouseButtonEvent {
            static_cast<input::MouseButton>(sdlEvent.button.button),
            sdlEvent.button.down,
            sdlEvent.button.clicks,
            scaleWinCoord(sdlEvent.button.x, _options.graphics.winScale),
            scaleWinCoord(sdlEvent.button.y, _options.graphics.winScale)});
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return input::Event::newMouseButtonUp(input::MouseButtonEvent {
            static_cast<input::MouseButton>(sdlEvent.button.button),
            sdlEvent.button.down,
            sdlEvent.button.clicks,
            scaleWinCoord(sdlEvent.button.x, _options.graphics.winScale),
            scaleWinCoord(sdlEvent.button.y, _options.graphics.winScale)});
    case SDL_EVENT_MOUSE_WHEEL: {
        return input::Event::newMouseWheel(input::MouseWheelEvent {
            sdlEvent.wheel.x,
            sdlEvent.wheel.y,
            static_cast<input::MouseWheelDirection>(sdlEvent.wheel.direction)});
    default:
        return std::nullopt;
    }
    }
}

} // namespace reone
