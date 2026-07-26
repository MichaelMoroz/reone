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
#include "reone/graphics/font.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/vulkan/gbuffer.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/mesh.h"
#include "reone/graphics/vulkan/pipeline.h"
#include "reone/graphics/vulkan/pipelinecache.h"
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
static std::shared_ptr<Mesh> makeCube(bool fullLayout = false) {
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

    if (!fullLayout) {
        // position, normal, uv1: 8 floats, in that order.
        auto layout = Mesh::VertexLayoutBuilder()
                          .stride(8 * sizeof(float))
                          .offPosition(0)
                          .offNormals(3 * sizeof(float))
                          .offUV1(6 * sizeof(float))
                          .build();
        return std::make_shared<Mesh>(std::move(vertices), std::move(layout),
                                      std::move(meshFaces));
    }

    // The real model shader declares every attribute, and a vertex input the
    // pipeline does not supply is an error rather than a default. So the full
    // layout the engine's models use: 28 floats.
    for (auto &vertex : vertices) {
        vertex.uv2 = *vertex.uv1;
        vertex.tangent = glm::vec3(1.0f, 0.0f, 0.0f);
        vertex.bitangent = glm::vec3(0.0f, 1.0f, 0.0f);
        vertex.tanSpaceNormal = glm::vec3(0.0f, 0.0f, 1.0f);
        vertex.boneIndices = glm::ivec4(0);
        // Full weight on bone 0, which is identity, so skinning is a no-op
        // rather than collapsing every vertex onto the origin.
        vertex.boneWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        vertex.material = 0;
    }
    auto full = Mesh::VertexLayoutBuilder()
                    .stride(28 * sizeof(float))
                    .offPosition(0)
                    .offNormals(3 * sizeof(float))
                    .offUV1(6 * sizeof(float))
                    .offUV2(8 * sizeof(float))
                    .offTanSpace(10 * sizeof(float))
                    .offBoneIndices(19 * sizeof(float))
                    .offBoneWeights(23 * sizeof(float))
                    .offMaterial(27 * sizeof(float))
                    .build();
    return std::make_shared<Mesh>(std::move(vertices), std::move(full), std::move(meshFaces));
}

/**
 * A font atlas built in code: a 16x16 grid of cells where each cell holds a
 * filled bar whose height varies with the character code. Enough for the text
 * path to be exercised - metrics, glyph rects, instanced draws - without a game
 * install, and distinctive enough that wrong glyph coordinates are obvious.
 */
static std::shared_ptr<Font> makeFont(I2DRenderer &renderer2d) {
    constexpr int kCells = 16;
    constexpr int kCell = 16;
    constexpr int kSize = kCells * kCell;

    auto pixels = std::make_shared<ByteBuffer>();
    pixels->resize(static_cast<size_t>(kSize) * kSize * 4, 0);
    for (int code = 0; code < kCells * kCells; ++code) {
        int cx = (code % kCells) * kCell;
        int cy = (code / kCells) * kCell;
        // Bar height cycles so adjacent characters differ visibly.
        int barHeight = 3 + (code % 10);
        for (int y = kCell - barHeight; y < kCell - 1; ++y) {
            for (int x = 2; x < kCell - 2; ++x) {
                size_t i = (static_cast<size_t>(cy + y) * kSize + cx + x) * 4;
                for (int c = 0; c < 4; ++c) {
                    (*pixels)[i + c] = static_cast<char>(0xff);
                }
            }
        }
    }

    Texture::Properties props;
    auto atlas = std::make_shared<Texture>("probe_font", TextureType::TwoDim, props);

    Texture::Features features;
    features.numChars = kCells * kCells;
    features.fontHeight = 0.16f; // Font multiplies by 100 to get pixels.
    for (int code = 0; code < features.numChars; ++code) {
        float u = (code % kCells) / static_cast<float>(kCells);
        float v = (code / kCells) / static_cast<float>(kCells);
        float s = 1.0f / kCells;
        // Upper-left has the larger v, matching how the readers store them.
        features.upperLeftCoords.push_back({u, v + s, 0.0f});
        features.lowerRightCoords.push_back({u + s, v, 0.0f});
    }
    atlas->setFeatures(std::move(features));
    atlas->setPixels(kSize, kSize, PixelFormat::RGBA8, Texture::Layer {pixels});

    auto font = std::make_shared<Font>(renderer2d);
    font->load(atlas);
    return font;
}

int main(int argc, char **argv) {
    namespace po = boost::program_options;

    int width, height, frames;
    bool validation, vsync;
    std::string capturePath, clearColor, spirvPath, drawColor;
    bool drawMesh, deferred, drawTwoD, realPbr, allVariants, drawGrass;

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
         "render the cube through a G-buffer and resolve it")                        //
        ("twod", po::value<bool>(&drawTwoD)->default_value(false),                   //
         "draw sprites and rectangles through the 2D renderer")                      //
        ("pbr", po::value<bool>(&realPbr)->default_value(false),                     //
         "render through the real pbr_model shader into the G-buffer")               //
        ("variants", po::value<bool>(&allVariants)->default_value(false),             //
         "draw every pbr_model geometry entry point, one cube each")                  //
        ("grass", po::value<bool>(&drawGrass)->default_value(false),                  //
         "draw instanced grass, the case SV_InstanceID broke on OpenGL");             //

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
        // --twod exercises the 2D renderer on its own; the 3D test draw would
        // simply cover it.
        bool draw3D = !drawTwoD || drawMesh || deferred;
        std::unique_ptr<VulkanPipeline> pipeline;
        std::unique_ptr<VulkanImage> checker;
        std::shared_ptr<Mesh> cube;
        std::shared_ptr<Texture> sprite;
        std::shared_ptr<Font> font;
        if (drawTwoD) {
            // A checkerboard as an engine Texture, so the upload path under
            // test is the real one rather than VulkanImage directly.
            constexpr int kSide = 8;
            auto pixels = std::make_shared<ByteBuffer>();
            pixels->resize(kSide * kSide * 4);
            for (int y = 0; y < kSide; ++y) {
                for (int x = 0; x < kSide; ++x) {
                    auto v = static_cast<char>(((x + y) % 2) ? 0xff : 0x50);
                    for (int c = 0; c < 3; ++c) {
                        (*pixels)[(y * kSide + x) * 4 + c] = v;
                    }
                    (*pixels)[(y * kSide + x) * 4 + 3] = static_cast<char>(0xff);
                }
            }
            sprite = std::make_shared<Texture>("probe_sprite", TextureType::TwoDim,
                                               Texture::Properties());
            sprite->setPixels(kSide, kSide, PixelFormat::RGBA8, Texture::Layer {pixels});
            font = makeFont(renderer.renderer2d());
        }
        std::unique_ptr<VulkanMesh> vkCube;
        std::unique_ptr<VulkanGBuffer> gbuffer;
        VkDescriptorSet resolveSet {VK_NULL_HANDLE};
        std::unique_ptr<VulkanPipeline> resolvePipeline;
        if (drawGrass) {
            realPbr = true;
        }
        if (allVariants) {
            realPbr = true;
        }
        if (realPbr) {
            deferred = true;
        }
        if (deferred) {
            drawMesh = true;
        }
        if (realPbr && spirvPath == "spirv/vktriangle.spv") {
            spirvPath = "spirv/pbr_model.spv";
        } else if (deferred && spirvPath == "spirv/vktriangle.spv") {
            spirvPath = "spirv/vkgbuffer.spv";
        } else if (drawMesh && spirvPath == "spirv/vktriangle.spv") {
            spirvPath = "spirv/vkmesh.spv";
        }
        if (!draw3D) {
            info("2D only");
        } else if (!std::filesystem::exists(spirvPath)) {
            info("No SPIR-V at " + spirvPath + " - clearing only");
        } else {
            VulkanPipeline::Config config;
            config.spirv = readSpirV(spirvPath);
            if (realPbr) {
                config.vertexEntry = "staticVertex";
                config.fragmentEntry = "opaqueFragment";
            } else {
                config.vertexEntry = deferred ? "geometryVertex"
                                              : (drawMesh ? "meshVertex" : "triangleVertex");
                config.fragmentEntry = deferred ? "geometryFragment"
                                                : (drawMesh ? "meshFragment" : "triangleFragment");
            }
            if (drawGrass) {
                // Grass draws one instanced quad per cluster. The shader
                // billboards it from the cluster position, so the mesh is a
                // unit quad and everything else comes from the uniform block.
                std::vector<Mesh::Vertex> quadVerts;
                const glm::vec2 corners[4] {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                const glm::vec2 quadUVs[4] {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
                for (int i = 0; i < 4; ++i) {
                    quadVerts.push_back(Mesh::VertexBuilder()
                                            .position({corners[i], 0.0f})
                                            .normal({0.0f, 0.0f, 1.0f})
                                            .uv1(quadUVs[i])
                                            .uv2(quadUVs[i])
                                            .tangent({1.0f, 0.0f, 0.0f})
                                            .bitangent({0.0f, 1.0f, 0.0f})
                                            .tanSpaceNormal({0.0f, 0.0f, 1.0f})
                                            .boneIndices(glm::ivec4(0))
                                            .boneWeights({1.0f, 0.0f, 0.0f, 0.0f})
                                            .material(0)
                                            .build());
                }
                std::vector<Mesh::Face> quadFaces {
                    Mesh::Face({0, 1, 2}), Mesh::Face({0, 2, 3})};
                auto quadLayout = Mesh::VertexLayoutBuilder()
                                      .stride(28 * sizeof(float))
                                      .offPosition(0)
                                      .offNormals(3 * sizeof(float))
                                      .offUV1(6 * sizeof(float))
                                      .offUV2(8 * sizeof(float))
                                      .offTanSpace(10 * sizeof(float))
                                      .offBoneIndices(19 * sizeof(float))
                                      .offBoneWeights(23 * sizeof(float))
                                      .offMaterial(27 * sizeof(float))
                                      .build();
                cube = std::make_shared<Mesh>(std::move(quadVerts), std::move(quadLayout),
                                              std::move(quadFaces));
            } else if (drawMesh) {
                cube = makeCube(realPbr);
            }
            if (cube) {
                // Whatever mesh was chosen, the pipeline must describe its
                // layout: an input the pipeline does not supply is an error.
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
                resolveConfig.spirv = realPbr ? readSpirV("spirv/vkgbuffer.spv") : config.spirv;
                resolveConfig.vertexEntry = "resolveVertex";
                resolveConfig.fragmentEntry = "resolveFragment";
                resolveConfig.colorFormats = {renderer.swapchain().imageFormat()};
                resolveConfig.setLayouts = config.setLayouts;
                resolvePipeline = std::make_unique<VulkanPipeline>(renderer.device());
                resolvePipeline->init(resolveConfig);

                // The resolve samples the G-buffer at units 1..4. It gets its
                // own set: putting these in the standing bindings would make
                // the geometry pass bind descriptors pointing at images that
                // are colour attachments at that moment.
                std::vector<std::pair<int, const VulkanImage *>> resolveTextures;
                for (int i = 0; i < VulkanGBuffer::Count - 1; ++i) {
                    resolveTextures.push_back({i + 1, &gbuffer->color(i)});
                }
                resolveSet = renderer.descriptors().createPersistentTextureSet(resolveTextures);
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
        int twoDDraws = 0;
        int grassInstances = 0;
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
                    auto eye = drawGrass ? glm::vec3(0.0f, 2.0f, 12.0f)
                                         : glm::vec3(0.0f, 0.0f, allVariants ? 14.0f : 6.0f);
                    auto view = glm::lookAt(eye,
                                            drawGrass ? glm::vec3(0.0f, -1.0f, 0.0f)
                                                      : glm::vec3(0.0f),
                                            glm::vec3(0.0f, 1.0f, 0.0f));
                    auto proj = glm::perspective(glm::radians(45.0f),
                                                 w / static_cast<float>(h), 0.1f, 100.0f);
                    // Vulkan clip space has y down relative to OpenGL's.
                    proj[1][1] *= -1.0f;
                    globals.viewProjection = proj * view;
                    globals.view = view;
                    globals.viewInv = glm::inverse(view);
                    globals.projection = proj;
                    globals.projectionInv = glm::inverse(proj);
                    globals.cameraPosition = glm::vec4(0.0f, 0.0f, 6.0f, 1.0f);
                    if (realPbr) {
                        // No feature bits: plain diffuse from the main texture,
                        // world normal straight from the vertex, no lightmap.
                        locals.featureMask = 0;
                        locals.modelInv = glm::inverse(locals.model);
                        locals.prevModel = locals.model;
                        locals.selfIllumColor = glm::vec4(0.0f);
                        globals.prevViewProjection = globals.viewProjection;
                    }
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

                auto textureSet = renderer.descriptors().acquireTextureSet(
                    renderer.uniformRing().frame(), nullptr);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline->layout(), VulkanDescriptors::kTextureSet,
                                        1, &textureSet, 0, nullptr);
                if (vkCube && drawGrass) {
                    VulkanPipelineCache::Key key;
                    key.module = "grass";
                    key.vertexEntry = "grassVertex";
                    key.fragmentEntry = "pbrFragment";
                    key.colorFormats = VulkanGBuffer::colorFormats();
                    key.depthFormat = VulkanGBuffer::depthFormat();
                    key.depthTest = true;
                    key.depthWrite = true;
                    key.vertexBindings = {VulkanMesh::bindingDescription(cube->vertexLayout())};
                    key.vertexAttributes = VulkanMesh::attributeDescriptions(cube->vertexLayout());
                    auto &grassPipeline = renderer.pipelines().get(key);

                    // A grid of clusters. If SV_InstanceID works, this is a
                    // field; if it reads zero, as it did under OpenGL, every
                    // quad lands on cluster 0 and there is one billboard.
                    GrassUniforms grass;
                    grass.quadSize = glm::vec2(0.6f);
                    grass.radius = 40.0f;
                    int side = 16;
                    int count = side * side;
                    for (int i = 0; i < count && i < kMaxGrassClusters; ++i) {
                        float gx = (i % side) - side * 0.5f;
                        float gz = (i / side) - side * 0.5f;
                        grass.clusters[i].positionVariant =
                            glm::vec4(gx * 0.7f, -1.5f, gz * 0.7f, 0.0f);
                        grass.clusters[i].lightmapUV = glm::vec4(0.0f);
                    }

                    LocalUniforms glocals = locals;
                    glocals.model = glm::mat4(1.0f);
                    glocals.modelInv = glm::mat4(1.0f);
                    glocals.prevModel = glm::mat4(1.0f);

                    std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> goff = offsets;
                    goff[UniformBlockBindingPoints::locals] =
                        renderer.uniformRing().push(glocals);
                    goff[UniformBlockBindingPoints::grass] =
                        renderer.uniformRing().push(grass);

                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      grassPipeline.handle());
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            grassPipeline.layout(),
                                            VulkanDescriptors::kUniformSet, 1, &uniformSet,
                                            static_cast<uint32_t>(goff.size()), goff.data());
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            grassPipeline.layout(),
                                            VulkanDescriptors::kTextureSet, 1, &textureSet,
                                            0, nullptr);
                    vkCube->draw(cmd, count);
                    grassInstances = count;
                } else if (vkCube && allVariants) {
                    // Every geometry entry point in the shipping model shader.
                    // Each is a separate pipeline; the cache builds them once.
                    static const char *kEntries[] {
                        "staticVertex", "skinnedVertex", "danglyVertex", "saberVertex"};
                    VulkanPipelineCache::Key key;
                    key.module = "pbr_model";
                    key.fragmentEntry = "opaqueFragment";
                    key.colorFormats = VulkanGBuffer::colorFormats();
                    key.depthFormat = VulkanGBuffer::depthFormat();
                    key.depthTest = true;
                    key.depthWrite = true;
                    key.vertexBindings = {VulkanMesh::bindingDescription(cube->vertexLayout())};
                    key.vertexAttributes = VulkanMesh::attributeDescriptions(cube->vertexLayout());

                    for (int v = 0; v < 4; ++v) {
                        key.vertexEntry = kEntries[v];
                        auto &variant = renderer.pipelines().get(key);

                        LocalUniforms vlocals = locals;
                        vlocals.model = glm::translate(
                                            glm::vec3(-4.5f + v * 3.0f, 0.0f, 0.0f)) *
                                        locals.model;
                        vlocals.modelInv = glm::inverse(vlocals.model);
                        vlocals.prevModel = vlocals.model;

                        std::array<uint32_t, VulkanDescriptors::kNumUniformBlocks> voff = offsets;
                        voff[UniformBlockBindingPoints::locals] =
                            renderer.uniformRing().push(vlocals);
                        // Identity bones and no dangly displacement: enough for
                        // the variants to run without a scene behind them.
                        voff[UniformBlockBindingPoints::bones] =
                            renderer.uniformRing().push(BoneUniforms {});
                        // danglyVertex takes its position wholly from this
                        // block, ignoring the vertex buffer, so a zeroed block
                        // collapses the mesh onto the origin. Feed it the
                        // cube's own vertices; a real dangly mesh gets these
                        // from CPU-side deformation.
                        DanglyUniforms dangly;
                        for (int i = 0; i < cube->vertexCount() &&
                                        i < kMaxDanglyVertices; ++i) {
                            const auto &pos = cube->vertexCoords()[i];
                            dangly.positions[i] = glm::vec4(pos, 1.0f);
                        }
                        voff[UniformBlockBindingPoints::dangly] =
                            renderer.uniformRing().push(dangly);

                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, variant.handle());
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                variant.layout(),
                                                VulkanDescriptors::kUniformSet, 1, &uniformSet,
                                                static_cast<uint32_t>(voff.size()), voff.data());
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                variant.layout(),
                                                VulkanDescriptors::kTextureSet, 1, &textureSet,
                                                0, nullptr);
                        vkCube->draw(cmd);
                    }
                } else if (vkCube) {
                    vkCube->draw(cmd);
                } else {
                    vkCmdDraw(cmd, 3, 1, 0, 0);
                }
                vkCmdEndRendering(cmd);

                if (deferred) {
                    // Attachments become textures. Without this barrier the
                    // resolve may sample them before the writes have landed.
                    gbuffer->transitionColor(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

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
                                            VulkanDescriptors::kTextureSet, 1, &resolveSet,
                                            0, nullptr);
                    vkCmdDraw(cmd, 3, 1, 0, 0);
                    vkCmdEndRendering(cmd);

                    // Back to attachment layout for the next frame's clear.
                    gbuffer->transitionColor(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                }
            }

            if (drawTwoD) {
                auto cmd = renderer.commandBuffer();
                VkRenderingAttachmentInfo attachment {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                attachment.imageView = renderer.currentImageView();
                attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                VkRenderingInfo rendering {VK_STRUCTURE_TYPE_RENDERING_INFO};
                rendering.renderArea.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &attachment;

                VkViewport vp2 {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
                VkRect2D sc2 {{0, 0}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};

                vkCmdBeginRendering(cmd, &rendering);
                vkCmdSetViewport(cmd, 0, 1, &vp2);
                vkCmdSetScissor(cmd, 0, 1, &sc2);

                auto &r2d = renderer.renderer2d();
                r2d.begin(cmd, {w, h}, renderer.swapchain().imageFormat());
                // Opaque sprite, tinted sprite, a solid bar, and a scissored
                // sprite - the four things every GUI element is made of.
                r2d.drawImage(*sprite, {40.0f, 40.0f}, {200.0f, 200.0f});
                r2d.drawImage(*sprite, {260.0f, 40.0f}, {200.0f, 200.0f},
                              glm::vec4(1.0f, 0.4f, 0.2f, 1.0f));
                r2d.drawRect({40.0f, 260.0f}, {420.0f, 40.0f},
                             glm::vec4(0.2f, 0.8f, 0.4f, 1.0f));
                r2d.withScissor({40, 320, 200, 100}, [&r2d, &sprite]() {
                    r2d.drawImage(*sprite, {40.0f, 320.0f}, {420.0f, 200.0f});
                });
                // Font::render forwards to the 2D renderer, so this is the
                // same call the game makes.
                font->render("reone vulkan 2d", glm::vec3(40.0f, 460.0f, 0.0f),
                             glm::vec3(1.0f, 0.9f, 0.5f), TextGravity::RightCenter);
                r2d.withBlendMode(BlendMode::Additive, [&r2d, &sprite]() {
                    r2d.drawImage(*sprite, {480.0f, 40.0f}, {200.0f, 200.0f},
                                  glm::vec4(0.3f, 0.5f, 1.0f, 1.0f));
                });
                r2d.end();
                vkCmdEndRendering(cmd);
                twoDDraws = r2d.drawCount();
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
        info(str(boost::format("Pipelines built: %d") % renderer.pipelines().size()));
        if (drawGrass) {
            info(str(boost::format("Grass instances: %d") % grassInstances));
        }
        if (drawTwoD) {
            info(str(boost::format("2D draws per frame: %d") % twoDDraws));
        }
    } catch (const std::exception &e) {
        error(std::string("Vulkan probe failed: ") + e.what());
        exitCode = 1;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}
