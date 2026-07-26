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
#include "reone/graphics/mesh.h"
#include "reone/graphics/types.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/vulkan/gbuffer.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/mesh.h"
#include "reone/graphics/vulkan/pipeline.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/system/stream/fileoutput.h"
#include "reone/system/logger.h"
#include "reone/system/logutil.h"

using namespace reone;
using namespace reone::graphics;

/**
 * A unit cube as a Mesh, built the way the game builds meshes: interleaved
 * vertex data plus a VertexLayout describing it. Each face gets its own four
 * vertices so the normals are flat.
 */
static std::shared_ptr<Mesh> makeCube() {
    struct Face {
        glm::vec3 normal;
        glm::vec3 corners[4];
    };
    const Face faces[] {
        {{0, 0, 1}, {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}},
        {{0, 0, -1}, {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}}},
        {{1, 0, 0}, {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}}},
        {{-1, 0, 0}, {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}}},
        {{0, 1, 0}, {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}}},
        {{0, -1, 0}, {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}}},
    };
    const glm::vec2 uvs[4] {{0, 0}, {1, 0}, {1, 1}, {0, 1}};

    std::vector<Mesh::Vertex> vertices;
    std::vector<Mesh::Face> meshFaces;
    for (const auto &face : faces) {
        auto base = static_cast<uint16_t>(vertices.size());
        for (int i = 0; i < 4; ++i) {
            vertices.push_back(Mesh::VertexBuilder()
                                   .position(face.corners[i])
                                   .normal(face.normal)
                                   .uv1(uvs[i])
                                   .build());
        }
        meshFaces.push_back(Mesh::Face({base, static_cast<uint16_t>(base + 1), static_cast<uint16_t>(base + 2)}));
        meshFaces.push_back(Mesh::Face({base, static_cast<uint16_t>(base + 2), static_cast<uint16_t>(base + 3)}));
    }

    // position, normal, uv1: 8 floats, in that order.
    auto layout = Mesh::VertexLayoutBuilder()
                      .stride(8 * sizeof(float))
                      .offPosition(0)
                      .offNormals(3 * sizeof(float))
                      .offUV1(6 * sizeof(float))
                      .build();
    return std::make_shared<Mesh>(std::move(vertices), std::move(layout), std::move(meshFaces));
}

int main(int argc, char **argv) {
    namespace po = boost::program_options;

    int width, height, frames;
    bool validation, vsync;
    std::string capturePath, clearColor, spirvPath, drawColor;
    bool drawMesh, deferred;

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
         "draw colour, pushed through the uniform ring")                            //
        ("mesh", po::value<bool>(&drawMesh)->default_value(false),                  //
         "draw a cube with real vertex attributes instead of the triangle")         //
        ("deferred", po::value<bool>(&deferred)->default_value(false),               //
         "render the cube through a G-buffer and resolve it");                       //

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
        std::unique_ptr<VulkanImage> checker;
        std::shared_ptr<Mesh> cube;
        std::unique_ptr<VulkanMesh> vkCube;
        std::unique_ptr<VulkanGBuffer> gbuffer;
        std::unique_ptr<VulkanPipeline> resolvePipeline;
        if (deferred) {
            drawMesh = true;
        }
        if (deferred && spirvPath == "spirv/vktriangle.spv") {
            spirvPath = "spirv/vkgbuffer.spv";
        } else if (drawMesh && spirvPath == "spirv/vktriangle.spv") {
            spirvPath = "spirv/vkmesh.spv";
        }
        if (!std::filesystem::exists(spirvPath)) {
            info("No SPIR-V at " + spirvPath + " - clearing only");
        } else {
            VulkanPipeline::Config config;
            config.spirv = readSpirV(spirvPath);
            config.vertexEntry = deferred ? "geometryVertex"
                                          : (drawMesh ? "meshVertex" : "triangleVertex");
            config.fragmentEntry = deferred ? "geometryFragment"
                                            : (drawMesh ? "meshFragment" : "triangleFragment");
            if (drawMesh) {
                cube = makeCube();
                config.vertexBindings = {VulkanMesh::bindingDescription(cube->vertexLayout())};
                config.vertexAttributes = VulkanMesh::attributeDescriptions(cube->vertexLayout());
            }
            if (deferred) {
                config.colorFormats = VulkanGBuffer::colorFormats();
                config.depthFormat = VulkanGBuffer::depthFormat();
                config.depthTest = true;
                config.depthWrite = true;
            } else {
                config.colorFormats = {renderer.swapchain().imageFormat()};
                if (drawMesh) {
                    config.depthFormat = renderer.depthFormat();
                    config.depthTest = true;
                    config.depthWrite = true;
                }
            }
            config.setLayouts = {renderer.descriptors().uniformLayout(),
                                 renderer.descriptors().textureLayout()};
            pipeline = std::make_unique<VulkanPipeline>(renderer.device());
            pipeline->init(config);
            info("Pipeline built from " + spirvPath);

            // A checkerboard, so a wrong sampler or a wrong layout shows up as
            // an obviously wrong image rather than a plausible flat colour.
            constexpr int kSide = 8;
            std::vector<uint32_t> texels(kSide * kSide);
            for (int y = 0; y < kSide; ++y) {
                for (int x = 0; x < kSide; ++x) {
                    texels[y * kSide + x] = ((x + y) % 2) ? 0xffffffff : 0xff404040;
                }
            }
            checker = std::make_unique<VulkanImage>(renderer.device());
            checker->initSampled2D({kSide, kSide}, VK_FORMAT_R8G8B8A8_UNORM, texels.data());
            renderer.descriptors().setTexture(TextureUnits::mainTex, *checker);

            if (deferred) {
                gbuffer = std::make_unique<VulkanGBuffer>(renderer.device());
                gbuffer->init({width, height});

                VulkanPipeline::Config resolveConfig;
                resolveConfig.spirv = config.spirv;
                resolveConfig.vertexEntry = "resolveVertex";
                resolveConfig.fragmentEntry = "resolveFragment";
                resolveConfig.colorFormats = {renderer.swapchain().imageFormat()};
                resolveConfig.setLayouts = config.setLayouts;
                resolvePipeline = std::make_unique<VulkanPipeline>(renderer.device());
                resolvePipeline->init(resolveConfig);

                // The resolve samples the G-buffer, so its attachments occupy
                // texture units 1..4 alongside the diffuse map at 0.
                for (int i = 0; i < VulkanGBuffer::Count - 1; ++i) {
                    renderer.descriptors().setTexture(i + 1, gbuffer->color(i));
                }
                info("G-buffer: 5 attachments at " +
                     std::to_string(width) + "x" + std::to_string(height));
            }

            if (drawMesh) {
                vkCube = std::make_unique<VulkanMesh>(renderer.device());
                vkCube->init(*cube);
                info(str(boost::format("Cube uploaded: %d indices, stride %d, %d attributes") %
                         vkCube->indexCount() % cube->vertexLayout().stride %
                         VulkanMesh::attributeDescriptions(cube->vertexLayout()).size()));
            }
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

                GlobalUniforms globals;
                globals.reset();
                if (drawMesh) {
                    // Spin it, so a capture shows the shape rather than a
                    // silhouette that could be a flat quad.
                    float angle = glm::radians(35.0f) + frame * 0.01f;
                    locals.model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.3f, 1.0f, 0.1f));
                    auto view = glm::lookAt(glm::vec3(0.0f, 0.0f, 6.0f),
                                            glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
                    auto proj = glm::perspective(glm::radians(45.0f),
                                                 w / static_cast<float>(h), 0.1f, 100.0f);
                    // Vulkan clip space has y down relative to OpenGL's.
                    proj[1][1] *= -1.0f;
                    globals.viewProjection = proj * view;
                }

                // Every binding in the set is dynamic, so every one needs an
                // offset even if the shader does not read it.
                std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> offsets {};
                offsets[UniformBlockBindingPoints::globals] = renderer.uniformRing().push(globals);
                offsets[UniformBlockBindingPoints::locals] = renderer.uniformRing().push(locals);

                auto cmd = renderer.commandBuffer();

                std::array<VkRenderingAttachmentInfo, VulkanGBuffer::Count> gbufAttachments {};
                if (deferred) {
                    for (int i = 0; i < VulkanGBuffer::Count; ++i) {
                        auto &a = gbufAttachments[i];
                        a.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                        a.imageView = gbuffer->color(i).view();
                        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    }
                }

                VkRenderingAttachmentInfo colorAttachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                colorAttachment.imageView = renderer.currentImageView();
                colorAttachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                // Load, not clear: beginFrame already cleared, and this proves
                // the triangle is drawn over it rather than replacing it.
                colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                VkRenderingAttachmentInfo depthAttachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                depthAttachment.imageView = renderer.depthView();
                depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                depthAttachment.clearValue.depthStencil.depth = 1.0f;

                VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
                rendering.renderArea.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &colorAttachment;
                if (drawMesh) {
                    rendering.pDepthAttachment = &depthAttachment;
                }
                if (deferred) {
                    // The geometry pass writes the G-buffer instead of the
                    // swapchain, with its own depth.
                    depthAttachment.imageView = gbuffer->depth().view();
                    rendering.colorAttachmentCount = VulkanGBuffer::Count;
                    rendering.pColorAttachments = gbufAttachments.data();
                }

                vkCmdBeginRendering(cmd, &rendering);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->handle());

                VkViewport vp {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
                VkRect2D scissor {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                auto uniformSet = renderer.uniformSet();
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline->layout(), VulkanDescriptors::kUniformSet,
                                        1, &uniformSet,
                                        static_cast<uint32_t>(offsets.size()), offsets.data());

                auto textureSet = renderer.descriptors().textureSet();
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline->layout(), VulkanDescriptors::kTextureSet,
                                        1, &textureSet, 0, nullptr);
                if (vkCube) {
                    vkCube->draw(cmd);
                } else {
                    vkCmdDraw(cmd, 3, 1, 0, 0);
                }
                vkCmdEndRendering(cmd);

                if (deferred) {
                    // Attachments become textures. Without this barrier the
                    // resolve may sample them before the writes have landed.
                    gbuffer->transitionColor(cmd,
                                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

                    VkRenderingInfo resolveRendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
                    resolveRendering.renderArea.extent = {static_cast<uint32_t>(w),
                                                          static_cast<uint32_t>(h)};
                    resolveRendering.layerCount = 1;
                    resolveRendering.colorAttachmentCount = 1;
                    resolveRendering.pColorAttachments = &colorAttachment;

                    vkCmdBeginRendering(cmd, &resolveRendering);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      resolvePipeline->handle());
                    vkCmdSetViewport(cmd, 0, 1, &vp);
                    vkCmdSetScissor(cmd, 0, 1, &scissor);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            resolvePipeline->layout(),
                                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                                            static_cast<uint32_t>(offsets.size()), offsets.data());
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            resolvePipeline->layout(),
                                            VulkanDescriptors::kTextureSet, 1, &textureSet,
                                            0, nullptr);
                    vkCmdDraw(cmd, 3, 1, 0, 0);
                    vkCmdEndRendering(cmd);

                    // Back to attachment layout for the next frame's clear.
                    gbuffer->transitionColor(cmd,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                }
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
        // Everything below this scope is destroyed before the renderer, so the
        // GPU has to be finished with it first. Runs that used --capture hid
        // this, because the capture path already waits on the queue.
        renderer.device().waitIdle();

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
