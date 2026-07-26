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
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "renderdoc_app.h"

#include "reone/graphics/format/tgawriter.h"
#include "reone/graphics/window.h"
#ifdef R_ENABLE_VULKAN
#include "reone/graphics/vulkan/debugscope.h"
#endif
#include "reone/system/randomutil.h"
#include "reone/system/stream/fileoutput.h"
#include "reone/resource/exception/notfound.h"
#include "reone/resource/gameprobe.h"

#include "editor.h"

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

static void imguiInit() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::GetStyle().FontScaleMain = 1.5f;
}

static void imguiInitWindow(Window &window) {
    ImGui_ImplSDL3_InitForOpenGL(window.sdlWindow(), window.sdlContext());
    ImGui_ImplOpenGL3_Init();
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

static void imguiNewFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (!ImGui::GetIO().WantCaptureMouse) {
        // Hand the cursor back to the game once it leaves an ImGui window.
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
}

static void imguiRender() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static void imguiShutdown() {
    // deinit runs from the destructor, and also after a failed init, so this
    // must tolerate being called when no context was ever created.
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
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
    _vulkan = _options.backend == "vulkan";
    if (_vulkan) {
#ifndef R_ENABLE_VULKAN
        throw std::runtime_error("--backend vulkan requires a build with ENABLE_VULKAN");
#endif
        // Before the window: the two backends need different window flags, and
        // asset objects consult this when deciding whether to make GL calls.
        setCurrentBackend(GraphicsBackend::Vulkan);
    }
    _window = std::make_unique<Window>(_options.graphics);
    _window->init();

    if (!_vulkan) {
        // The ImGui backends are OpenGL ones. The editor is unavailable on
        // Vulkan until they are replaced.
        imguiInit();
        imguiInitWindow(*_window);
    }

    if (_options.randomSeed >= 0) {
        seedRandom(static_cast<uint32_t>(_options.randomSeed));
    } else if (isCaptureRun()) {
        // Any run that produces something to compare needs the same sequence
        // every time, whether it writes a screenshot, a target dump, or both.
        seedRandom(0);
    }

    _optionsView = _options.toView();
    GameProbe probe {_options.game.path};
    auto gameId = probe.probe();

    _clock = std::make_unique<Clock>();
    _clock->init();

    _systemModule = std::make_unique<SystemModule>(*_clock);
    _graphicsModule = std::make_unique<GraphicsModule>(_options.graphics, _window.get());
#ifdef R_ENABLE_VULKAN
    if (_vulkan) {
        _vulkanRenderer = std::make_unique<VulkanRenderer>(
            _window->sdlWindow(),
            glm::ivec2 {_options.graphics.width, _options.graphics.height},
            _options.graphics.vsync,
            _options.vulkanValidation);
        _vulkanRenderer->init();
        _graphicsModule->setRenderers(*_vulkanRenderer, _vulkanRenderer->renderer2d());
    }
#endif
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

#ifdef R_ENABLE_VULKAN
    if (_vulkan) {
        // The scene library cannot reach the renderer on its own; the engine
        // owns it and hands it over so a Vulkan pipeline can be built.
        _sceneModule->renderPipelineFactory().setVulkanRenderer(*_vulkanRenderer);
    }
#endif

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

    if (!_vulkan) {
        // Editor is built on the ImGui OpenGL backend, which is not initialised
        // under Vulkan.
        _editor = std::make_unique<Editor>(*this);
    }

    if (_options.commandsFrame == 0) {
        runCommandsFile();
    }
}

void Engine::deinit() {
    _editor.reset();
    imguiShutdown();

    _console.reset();
    _profiler.reset();
    _game.reset();
    _services.reset();

    _gameModule.reset();
    _guiModule.reset();
    _sceneModule.reset();
    _resourceModule.reset();
    _scriptModule.reset();
    _movieModule.reset();
    _audioModule.reset();
    _graphicsModule.reset();
    _systemModule.reset();
    _clock.reset();

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
        if (!_window->isInFocus() && !isCaptureRun()) {
            std::this_thread::sleep_for(std::chrono::milliseconds {100});
            continue;
        }
        uint64_t ticks = clock.micros();
        auto frameTime = (ticks - _ticks) / 10e5f;
        _ticks = ticks;
        if (isCaptureRun()) {
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
            runCommandsFile();
        }
        _profiler->measure(kMainThreadName, kProfilerUpdateTimeIndex, [this, &frameTime]() {
            if (!_vulkan) {
                imguiNewFrame();
            }
            _game->update(frameTime);
            bool showcur = _game->cursorType() == CursorType::None;
            bool relmouse = _game->relativeMouseMode();
            if (_editor && _editor->isEnabled()) {
                // The in-game camera grabs the pointer, which would make editor
                // windows unreachable. Release it for as long as the editor is up.
                // Cursor visibility is left to ImGui, which drives it every frame
                // from the cursor imguiNewFrame selects.
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
            renderFrame(quit);
        });
        _profiler->measure(kMainThreadName, kProfilerRenderAudioTimeIndex, [this]() {
            _services->audio.mixer.render();
        });
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
    if (!isCaptureRun() || _captured) {
        return;
    }
    if (_frameIndex < _options.captureFrame) {
        // Ask RenderDoc for the frame before the one we screenshot, so the
        // capture holds a complete frame rather than one cut short by the exit.
        if (_options.renderdoc && !_renderdocTriggered &&
            _frameIndex + 1 >= _options.captureFrame) {
            if (auto api = renderdocApi()) {
                api->TriggerCapture();
                info("RenderDoc capture triggered");
                _renderdocTriggered = true;
            } else {
                warn("--renderdoc given but the process is not running under RenderDoc");
                _renderdocTriggered = true;
            }
        }
        return;
    }
    if (!_options.capturePath.empty()) {
        // Read before endFrame, while the finished frame is still readable.
        auto screenshot = _services->graphics.renderer.captureFrame();
        auto stream = FileOutputStream(_options.capturePath);
        TgaWriter(screenshot).save(stream);
        info("Wrote screenshot: " + _options.capturePath);
    }
    _captured = true;
    dumpTargetsIfRequested();
    quit = true;
}

void Engine::dumpTargetsIfRequested() {
    if (_options.dumpTargetsPath.empty()) {
        return;
    }
    auto *pipeline = _services->scene.graphs.get(kSceneMain).renderPipeline();
    if (!pipeline) {
        warn("--dumptargets given but the main scene has not been rendered");
        return;
    }
    // The frame this describes has to be finished before its targets are read.
    // On Vulkan the work was only submitted; on OpenGL the driver may still be
    // several frames behind. Both are settled here rather than inside the dump,
    // because only the caller knows which frame it means.
    if (_vulkan) {
#ifdef R_ENABLE_VULKAN
        _vulkanRenderer->device().waitIdle();
#endif
    } else {
        glFinish();
    }
    pipeline->dumpTargets(_options.dumpTargetsPath);
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
    if (_vulkan) {
        renderVulkanFrame(quit);
    } else {
        renderGLFrame(quit);
    }
    _inFrame = false;
}

void Engine::renderGLFrame(bool &quit) {
    if (_options.graphics.pbr) {
        _services->graphics.pbrTextures.refresh();
    }
    _services->graphics.renderer.beginFrame(
        {_options.graphics.width, _options.graphics.height});
    // Scene targets are produced before anything 2D is drawn, on both backends,
    // so the two paths agree on when a scene may be rendered.
    _game->renderSceneOffscreen();
    _game->render();
    _profiler->render();
    _console->render();
    if (_editor) {
        _editor->render();
    }
    imguiRender();
    captureIfRequested(quit);
    _services->graphics.renderer.endFrame();
}

void Engine::renderVulkanFrame(bool &quit) {
#ifdef R_ENABLE_VULKAN
    glm::ivec2 extent {_options.graphics.width, _options.graphics.height};
    // Before the frame's rendering scope: the scene pipeline begins render
    // passes of its own, and one cannot be nested inside another.
    _vulkanRenderer->beginFrame(extent);
    _game->renderSceneOffscreen();

    // One rendering scope for the whole frame. Everything the game draws at
    // this point is 2D; the scene pipeline is not on Vulkan yet.
    auto cmd = _vulkanRenderer->commandBuffer();
    VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = _vulkanRenderer->currentImageView();
    attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = {static_cast<uint32_t>(extent.x),
                                   static_cast<uint32_t>(extent.y)};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;

    VkViewport viewport {0.0f, 0.0f, static_cast<float>(extent.x),
                         static_cast<float>(extent.y), 0.0f, 1.0f};
    VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(extent.x),
                               static_cast<uint32_t>(extent.y)}};

    {
        // Closed before endFrame ends the command buffer: a label scope that
        // outlives recording is a validation error, not a stray marker.
        graphics::VulkanDebugScope scope2d(_vulkanRenderer->device(), cmd,
                                           "2D (scene composite, GUI, console)",
                                           {0.9f, 0.9f, 0.4f});

        vkCmdBeginRendering(cmd, &rendering);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        auto &renderer2d = _vulkanRenderer->renderer2d();
        renderer2d.begin(cmd, extent, _vulkanRenderer->swapchain().imageFormat());
        _game->render();
        _console->render();
        renderer2d.end();

        vkCmdEndRendering(cmd);
    }

    captureIfRequested(quit);
    _vulkanRenderer->endFrame();
#endif
}

void Engine::runCommandsFile() {
    if (_options.commandsFile.empty()) {
        return;
    }
    std::ifstream file(_options.commandsFile);
    if (!file.good()) {
        throw std::runtime_error("Failed to open commands file: " + _options.commandsFile);
    }
    for (std::string line; std::getline(file, line);) {
        // getline keeps the carriage return of a CRLF file, which would end up
        // inside the last argument of every command.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            _console->execute(line);
        }
    }
}

void Engine::processEvents(bool &quit) {
    std::queue<input::Event> unhandled;
    SDL_Event sdlEvent;
    while (SDL_PollEvent(&sdlEvent)) {
        if (sdlEvent.type == SDL_EVENT_QUIT) {
            quit = true;
            break;
        }
        if (!_window->isAssociatedWith(sdlEvent)) {
            if (!_vulkan) {
                imguiHandle(sdlEvent);
            }
            continue;
        }
        if (_window->handle(sdlEvent)) {
            if (_window->isCloseRequested()) {
                quit = true;
                break;
            }
            continue;
        }
        auto event = eventFromSDLEvent(sdlEvent);
        if (!event) {
            continue;
        }
        if (isCaptureRun()) {
            // Dropped rather than handled. A single mouse move over the window
            // turns the camera, and from then on frame 900 is a different
            // frame - which is most of why two runs of the same build did not
            // match. Console commands still arrive, through the commands file
            // rather than through here.
            continue;
        }
        if (_profiler->handle(*event)) {
            continue;
        }
        if (_editor && _editor->handle(*event)) {
            continue;
        }
        // Last filter before the game sees it: ImGui only claims the event when
        // it actually wants the mouse or keyboard. There is no ImGui context
        // under Vulkan, and ImGui::GetIO() on a null context faults - which is
        // why the crash depended on whether an SDL event happened to arrive.
        if (!_vulkan && imguiHandle(sdlEvent)) {
            continue;
        }
        unhandled.push(*event);
    }
    while (!unhandled.empty()) {
        _events.push(std::move(unhandled.front()));
        unhandled.pop();
    }
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
