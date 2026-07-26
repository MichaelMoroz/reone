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
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/pipeline.h"
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
    std::string capturePath, clearColor, spirvPath, drawColor;

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
         "clear colour as comma-separated floats")                                  //
        ("spirv", po::value<std::string>(&spirvPath)->default_value("spirv/vktriangle.spv"), //
         "SPIR-V module to draw a test triangle with, if present")                  //
        ("draw", po::value<std::string>(&drawColor)->default_value("0.9,0.4,0.1"),  //
         "triangle colour, pushed through the uniform ring");                       //

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
        glm::vec4 draw {1.0f};
        {
            std::vector<std::string> parts;
            boost::split(parts, drawColor, boost::is_any_of(","));
            for (size_t i = 0; i < parts.size() && i < 3; ++i) {
                draw[static_cast<int>(i)] = std::stof(parts[i]);
            }
        }
        renderer.setClearColor(clear);
        renderer.init();

        // The draw is optional so the clear path can still be exercised alone.
        std::unique_ptr<VulkanPipeline> pipeline;
        if (!std::filesystem::exists(spirvPath)) {
            info("No SPIR-V at " + spirvPath + " - clearing only");
        } else {
            VulkanPipeline::Config config;
            config.spirv = readSpirV(spirvPath);
            config.vertexEntry = "triangleVertex";
            config.fragmentEntry = "triangleFragment";
            config.colorFormat = renderer.swapchain().imageFormat();
            config.setLayouts = {renderer.descriptors().uniformLayout()};
            pipeline = std::make_unique<VulkanPipeline>(renderer.device());
            pipeline->init(config);
            info("Pipeline built from " + spirvPath);
        }

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

            if (pipeline) {
                // A slice of this frame's arena, addressed by dynamic offset.
                // This is the model §3.1 of the plan calls for, exercised for
                // real: the colour the shader reads was written here.
                LocalUniforms locals;
                locals.reset();
                locals.color = draw;
                auto offset = renderer.uniformRing().push(locals);

                // Every binding in the set is dynamic, so every one needs an
                // offset even though only locals is read.
                std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
                offsets[UniformBlockBindingPoints::locals] = offset;

                auto cmd = renderer.commandBuffer();

                VkRenderingAttachmentInfo colorAttachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                colorAttachment.imageView = renderer.currentImageView();
                colorAttachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                // Load, not clear: beginFrame already cleared, and this proves
                // the triangle is drawn over it rather than replacing it.
                colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
                rendering.renderArea.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &colorAttachment;

                vkCmdBeginRendering(cmd, &rendering);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());

                VkViewport vp {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
                VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                auto set = renderer.uniformSet();
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline->layout(), 0, 1, &set,
                                        static_cast<uint32_t>(offsets.size()), offsets.data());
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);
            }

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
        info(str(boost::format("Uniform arena peak: %llu bytes") % renderer.uniformRing().peakUsage()));
    } catch (const std::exception &e) {
        error(std::string("Vulkan probe failed: ") + e.what());
        exitCode = 1;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
