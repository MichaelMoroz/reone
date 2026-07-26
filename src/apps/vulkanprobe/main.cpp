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

/**
 * A host for the Vulkan backend on its own.
 *
 * The engine cannot switch backends yet: OpenGL and Vulkan cannot share a
 * window, and every other subsystem still talks to the GL context, so pointing
 * the engine at Vulkan today means it dies during module init rather than
 * telling us anything about the backend. This runs the backend without the rest
 * of the engine attached, so device, swapchain and frame submission can be
 * brought up and validated on their own.
 *
 * It is scaffolding. It goes away once the engine can select a backend.
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <boost/program_options.hpp>

#include "reone/graphics/format/tgawriter.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/system/stream/fileoutput.h"
#include "reone/system/logger.h"
#include "reone/system/logutil.h"

using namespace reone;
using namespace reone::graphics;

int main(int argc, char **argv) {
    namespace po = boost::program_options;

    int width, height, frames;
    bool validation, vsync;
    std::string capturePath, clearColor;

    po::options_description desc("Options");
    desc.add_options()                                                              //
        ("help", "show this message")                                               //
        ("width", po::value<int>(&width)->default_value(1280), "window width")      //
        ("height", po::value<int>(&height)->default_value(720), "window height")    //
        ("frames", po::value<int>(&frames)->default_value(0),                       //
         "exit after this many frames, or 0 to run until closed")                   //
        ("validation", po::value<bool>(&validation)->default_value(true),           //
         "enable the Vulkan validation layers")                                     //
        ("vsync", po::value<bool>(&vsync)->default_value(true), "enable vsync")     //
        ("capture", po::value<std::string>(&capturePath)->default_value(""),        //
         "write a TGA of the last frame to this path")                              //
        ("clear", po::value<std::string>(&clearColor)->default_value("0,0,0"),      //
         "clear colour as comma-separated floats");                                 //

    po::variables_map vars;
    po::store(po::parse_command_line(argc, argv, desc), vars);
    po::notify(vars);
    if (vars.count("help")) {
        std::cout << desc << std::endl;
        return 0;
    }

    Logger::instance.init(LogSeverity::Info, {LogChannel::Global, LogChannel::Graphics}, std::nullopt);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        error("SDL_Init failed: " + std::string(SDL_GetError()));
        return 1;
    }
    auto window = SDL_CreateWindow("reone - Vulkan probe", width, height, SDL_WINDOW_VULKAN);
    if (!window) {
        error("SDL_CreateWindow failed: " + std::string(SDL_GetError()));
        SDL_Quit();
        return 1;
    }

    int exitCode = 0;
    try {
        VulkanRenderer renderer(window, {width, height}, vsync, validation);

        glm::vec4 clear {0.0f, 0.0f, 0.0f, 1.0f};
        {
            std::vector<std::string> parts;
            boost::split(parts, clearColor, boost::is_any_of(","));
            for (size_t i = 0; i < parts.size() && i < 3; ++i) {
                clear[static_cast<int>(i)] = std::stof(parts[i]);
            }
        }
        renderer.setClearColor(clear);
        renderer.init();

        int frame = 0;
        bool quit = false;
        while (!quit) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT ||
                    (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                    quit = true;
                }
            }
            if (quit) {
                break;
            }
            int w, h;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            if (w <= 0 || h <= 0) {
                // Minimised: a zero-extent swapchain is invalid, so idle.
                SDL_Delay(50);
                continue;
            }
            renderer.beginFrame({w, h});
            bool last = frames > 0 && frame + 1 >= frames;
            if (last && !capturePath.empty()) {
                auto shot = renderer.captureFrame();
                auto stream = FileOutputStream(capturePath);
                TgaWriter(shot).save(stream);
                info("Wrote screenshot: " + capturePath);
            }
            renderer.endFrame();

            if (frames > 0 && ++frame >= frames) {
                quit = true;
            }
        }
        info(str(boost::format("Presented %d frames on %s") % frame % renderer.device().deviceName()));
    } catch (const std::exception &e) {
        error(std::string("Vulkan probe failed: ") + e.what());
        exitCode = 1;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
