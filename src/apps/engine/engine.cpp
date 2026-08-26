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

static void imguiBeginFrame() {
    // Loading can request a frame before the main loop, while normal frames
    // begin before update so widgets submitted there belong to the render that
    // follows. Either path may reach the frame owner first.
    if (g_imguiFrameOpen) {
        return;
    }
    g_imguiRenderer->beginImGuiFrame();
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
        _options.vulkanValidation);
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
        ImGui::GetIO().IniFilename = nullptr;
        loadInputScript();
    }

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
        if (_options.freezeFrame > 0 && _frameIndex >= _options.freezeFrame) {
            // The world stops; the renderer does not. Everything temporal -
            // the jitter sequence, the tracer's frame index, NRD's history,
            // the TAA history - keeps advancing on a scene that no longer
            // moves, so whatever still changes between frames is the filters
            // failing to converge rather than the camera or an animation.
            frameTime = 0.0f;
            if (!_historyRestarted) {
                // Start the temporal filters cold on the first frozen frame.
                // A blend-factor filter settles at a small non-zero residual
                // rather than reaching zero, so the settled value alone proves
                // nothing; restarting here makes the approach to it visible,
                // and that geometric decay is the actual evidence.
                _historyRestarted = true;
                if (auto *pipeline = _services->scene.graphs.get(kSceneMain).renderPipeline()) {
                    pipeline->restartTemporalHistory();
                    info("Temporal history restarted at frame " + std::to_string(_frameIndex));
                }
            }
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
    requestGraphicsRebuild();
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
    while (!_scriptedCommands.empty()) {
        std::string command(std::move(_scriptedCommands.front()));
        _scriptedCommands.pop_front();
        _console->execute(command);
        if (_scriptQuitRequested) {
            quit = true;
            return;
        }
        if (_captureRequest || _scriptPauseFrames > 0) {
            return;
        }
    }
}

void Engine::processEvents(bool &quit) {
    std::queue<input::Event> unhandled;
    auto processEvent = [this, &quit, &unhandled](SDL_Event &sdlEvent, bool automated) {
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
    while (!quit && _nextAutomatedInput < _automatedInput.size() &&
           _automatedInput[_nextAutomatedInput].frame <= _frameIndex + 1) {
        auto event = _automatedInput[_nextAutomatedInput++].event;
        processEvent(event, true);
    }
    while (!unhandled.empty()) {
        _events.push(std::move(unhandled.front()));
        unhandled.pop();
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
        _automatedInput.push_back({frame, motion});

        SDL_Event down {};
        down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        down.button.windowID = windowId;
        down.button.button = SDL_BUTTON_LEFT;
        down.button.down = true;
        down.button.clicks = 1;
        down.button.x = static_cast<float>(x);
        down.button.y = static_cast<float>(y);
        _automatedInput.push_back({frame + 1, down});

        SDL_Event up = down;
        up.type = SDL_EVENT_MOUSE_BUTTON_UP;
        up.button.down = false;
        _automatedInput.push_back({frame + 2, up});
    };

    for (std::string line; std::getline(file, line);) {
        std::istringstream stream(line);
        int frame, x, y;
        std::string action;
        if (!(stream >> frame >> action) || (!action.empty() && action[0] == '#')) {
            continue;
        }
        if (action != "click" || !(stream >> x >> y) || frame < 1) {
            throw std::runtime_error("Invalid input script line: " + line);
        }
        addClick(frame, x, y);
    }
    std::stable_sort(_automatedInput.begin(), _automatedInput.end(),
                     [](const auto &lhs, const auto &rhs) { return lhs.frame < rhs.frame; });
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
