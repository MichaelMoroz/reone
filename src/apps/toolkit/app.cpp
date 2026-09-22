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

#include "app.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include "SDL3/SDL.h"

#include "renderdoc_app.h"

#include "reone/graphics/format/tgawriter.h"
#include "reone/resource/gameprobe.h"
#include "reone/system/logger.h"
#include "reone/system/randomutil.h"
#include "reone/system/threadutil.h"

#include "view/resource/explorerframe.h"

namespace reone {

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
        return getApi(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void **>(&result)) == 1 ? result : nullptr;
    }();
    return api;
#else
    return nullptr;
#endif
}

bool ToolkitApp::OnInit() {
#ifdef _WIN32
    SetProcessDPIAware();
#endif
    markMainThread();
    Logger::instance.init(
        LogSeverity::Debug,
        std::set<LogChannel> {LogChannel::Global, LogChannel::Resources, LogChannel::Graphics, LogChannel::Audio},
        "toolkit.log");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error("SDL_Init failed: " + std::string(SDL_GetError()));
    }
    wxImage::AddHandler(new wxTGAHandler);

    using namespace boost::program_options;
    options_description options {"Usage"};
    options.add_options()                                                                                              //
        ("game", value<std::string>(), "game resources directory")                                                  //
        ("open", value<std::string>(), "open a model resource by resref")                                           //
        ("capture", value<std::string>()->default_value(""), "write a preview screenshot to this path and exit")   //
        ("captureframe", value<int>()->default_value(3), "frame to capture on, counted from the first rendered frame") //
        ("renderdoc", value<bool>()->default_value(false), "trigger a RenderDoc frame capture with the screenshot"); //
    std::vector<std::string> commandLine;
    std::vector<const char *> commandLinePointers;
    commandLine.reserve(argc);
    commandLinePointers.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        commandLine.push_back(argv[i].ToStdString());
    }
    for (const auto &arg : commandLine) {
        commandLinePointers.push_back(arg.c_str());
    }
    variables_map vars;
    store(parse_command_line(static_cast<int>(commandLinePointers.size()), commandLinePointers.data(), options), vars);
    notify(vars);
    if (vars.count("game")) {
        m_captureOptions.gamePath = vars["game"].as<std::string>();
    }
    if (vars.count("open")) {
        m_captureOptions.resource = vars["open"].as<std::string>();
    }
    m_captureOptions.capturePath = vars["capture"].as<std::string>();
    m_captureOptions.captureFrame = vars["captureframe"].as<int>();
    m_captureOptions.renderdoc = vars["renderdoc"].as<bool>();
    if (m_captureOptions.isCaptureRun() && (m_captureOptions.gamePath.empty() || m_captureOptions.resource.empty())) {
        throw std::runtime_error("--capture requires both --game and --open");
    }
    if (m_captureOptions.captureFrame < 1) {
        throw std::runtime_error("--captureframe must be at least 1");
    }

    m_viewModel = std::make_unique<ResourceExplorerViewModel>();
    m_frame = new ResourceExplorerFrame {*m_viewModel, m_captureOptions.isCaptureRun()};
    m_frame->Show();
    if (!m_captureOptions.gamePath.empty()) {
        resource::GameProbe probe {m_captureOptions.gamePath};
        m_viewModel->onResourcesDirectoryChanged(probe.probe(), m_captureOptions.gamePath);
    }
    if (!m_captureOptions.resource.empty()) {
        m_viewModel->openModelByResRef(m_captureOptions.resource);
    }
    if (m_captureOptions.isCaptureRun()) {
        // Same deterministic basis as Engine::isCaptureRun: a fixed timestep
        // below, no interactive preview updates, and the shared generator at 0.
        setRandomSeed(0);
        CallAfter(&ToolkitApp::startCapture);
    }
    return true;
}

void ToolkitApp::startCapture() {
    m_captureTimer.SetOwner(this);
    Bind(wxEVT_TIMER, &ToolkitApp::onCaptureTimer, this, m_captureTimer.GetId());
    m_captureTimer.Start(1);
}

void ToolkitApp::onCaptureTimer(wxTimerEvent &event) {
    if (m_captureOptions.renderdoc && !m_renderdocTriggered &&
        m_frameIndex + 1 >= m_captureOptions.captureFrame) {
        if (auto api = renderdocApi()) {
            api->TriggerCapture();
            info("RenderDoc capture triggered");
        } else {
            warn("--renderdoc given but the process is not running under RenderDoc");
        }
        m_renderdocTriggered = true;
    }

    ++m_frameIndex;
    const auto *capturePath = m_frameIndex == m_captureOptions.captureFrame ? &m_captureOptions.capturePath : nullptr;
    m_frame->renderPreviewFrame(1.0f / 60.0f, capturePath);
    if (!capturePath) {
        return;
    }
    m_captureTimer.Stop();
    m_frame->Destroy();
    ExitMainLoop();
}

} // namespace reone

wxIMPLEMENT_APP(reone::ToolkitApp);
