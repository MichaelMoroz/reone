/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/graphics/vulkan/tracingpipeline.h"

#include "reone/system/profiler.h"

#include <algorithm>

#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/textureutil.h"
#include "reone/graphics/uniforms.h"

#include "reone/graphics/vulkan/buffer.h"
#include "reone/graphics/vulkan/descriptors.h"
#include "reone/graphics/vulkan/descriptorwrites.h"
#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/image.h"
#include "reone/graphics/vulkan/mesh.h"
#include "reone/graphics/vulkan/pipeline.h"
#include "reone/graphics/vulkan/pipelinecache.h"
#include "reone/graphics/vulkan/renderer.h"
#include "reone/graphics/vulkan/resources.h"
#include "reone/graphics/vulkan/tracingstructure.h"

#ifdef R_ENABLE_FSR
#include "reone/graphics/vulkan/fsrupscaler.h"
#endif
#ifdef R_ENABLE_NRD
#include "reone/graphics/vulkan/nrddenoiser.h"
#endif

#ifdef R_ENABLE_NRD
#include <NRD.h>
#endif
#include "reone/system/logutil.h"

#include <chrono>
#include <cstddef>

#include <glm/gtc/matrix_transform.hpp>

using namespace reone::graphics;

namespace reone::graphics {
namespace {

// Sky cubemap face resolution. Measured on danm14ab against the geometry sky
// it replaces, as a ratio of surviving horizontal detail: 512 keeps 0.59,
// 1024 keeps 0.73, 2048 keeps 0.77 for four times the memory. The curve is
// already flattening at 1024, so the rest of the gap is resampling and
// filtering rather than resolution, and paying 192 MB for it buys little.
// Frame time is flat across all three.
static constexpr uint32_t kSkyCubeSize = 1024;

struct TraceStats {
    uint32_t secondaryRays {0};
    uint32_t secondaryMisses {0};
    uint32_t survivingLights {0};
    uint32_t primaryHits {0};
    uint32_t shadowRays {0};
};

} // namespace

VulkanTracingPipeline::VulkanTracingPipeline(VulkanRenderer &renderer,
                                             glm::ivec2 extent,
                                             GraphicsOptions &options) :
    _renderer(renderer), _options(options), _extent(extent) {}

VulkanTracingPipeline::~VulkanTracingPipeline() {
    deinit();
}

void VulkanTracingPipeline::init() {
    if (_inited)
        return;
    auto &device = _renderer.device();
    _bindlessTextureCapacity = device.maxBindlessSampledImages();
    if (_bindlessTextureCapacity == 0) {
        throw std::runtime_error("Vulkan: ray-query bindless texture capacity is zero");
    }
    std::vector<VulkanPipeline::LayoutBinding> bindings {
        {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {{1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR},
        {{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR},
    };
    // Ranges into the one merged geometry buffer: vertices, indices, material ids.
    for (uint32_t i = 4; i <= 6; ++i) {
        bindings.push_back({{i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR});
    }
    bindings.push_back({{7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}, _bindlessTextureCapacity, VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    // The sky cube occupies binding 9, so the two bindless ranges keep their
    // fixed device-limit allocation rather than using Vulkan's highest-binding
    // variable-descriptor-count rule.
    bindings.push_back({{8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}, _bindlessTextureCapacity, VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT});
    bindings.push_back({{9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER}, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR});

    // A descriptor is required even when no room qualifies. Sampling is gated
    // in the shader, but this black cube keeps the descriptor type valid.
    const float black[4] {0.0f, 0.0f, 0.0f, 1.0f};
    _skyFallbackCube = std::make_unique<VulkanImage>(device);
    _skyFallbackCube->initSampledLayered({1, 1}, Format::R16G16B16A16Sfloat,
                                         kNumCubeFaces, true, black);
    _skyFallbackCube->setSampler(
        _renderer.resources().samplers().get(getTextureProperties(TextureUsage::ColorBuffer)));

    // The NRD output split: seven storage images in their own set, because
    // the main set's bindless arrays hold the variable-descriptor-count slot
    // and Vulkan allows nothing above it. Plain pool, static writes - the
    // images never change identity within a pipeline lifetime.
    {
        std::vector<VulkanPipeline::LayoutBinding> auxBindings;
        auxBindings.reserve(kNumAuxImages);
        for (uint32_t i = 0; i < kNumAuxImages; ++i) {
            auxBindings.push_back({{i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR});
        }
        VulkanPipeline::Config pipelineConfig;
        pipelineConfig.type = VulkanPipeline::Config::Type::RayTracing;
        pipelineConfig.spirv = _renderer.shaderModule("rayquery");
        pipelineConfig.raygenEntry = "main";
        pipelineConfig.descriptorSets = {{_renderer.descriptors().uniformLayout()},
                                         {VK_NULL_HANDLE, std::move(bindings), 2,
                                          VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT},
                                         {VK_NULL_HANDLE, std::move(auxBindings), 2}};
        pipelineConfig.pushConstantSize = sizeof(TracePushConstants);
        _pipeline = std::make_unique<VulkanPipeline>(device);
        _pipeline->init(pipelineConfig);
        // Formats mirror the shader's declarations; normal/roughness rides
        // RGBA16F, the FP form NRD's RGBA16_SNORM encoding accepts.
        static constexpr Format kAuxFormats[kNumAuxImages] {
            Format::R16G16B16A16Sfloat, // diffuse radiance + hit dist
            Format::R16G16B16A16Sfloat, // specular radiance + hit dist
            Format::R16G16B16A16Sfloat, // normal + roughness
            Format::R32Sfloat,          // viewZ
            Format::R16G16B16A16Sfloat, // motion
            Format::R16G16B16A16Sfloat, // noise-free
            Format::R16G16B16A16Sfloat, // diffuse material factor
            Format::R32Sfloat,          // device depth, for the upscaler
            Format::R16G16B16A16Sfloat, // screen-space motion, for the upscaler
            Format::R16G16B16A16Sfloat, // specular material factor
            Format::R8G8B8A8Unorm,      // canonical raster/tracer diffuse
            Format::R8G8B8A8Unorm,      // canonical packed eye normal
            Format::R32Sfloat,          // canonical positive linear view depth
            Format::R16G16Sfloat,       // canonical current-minus-previous UV motion
        };
        DescriptorWriteBuilder auxWrites(device.handle());
        for (int frame = 0; frame < 2; ++frame) {
            for (int i = 0; i < kNumAuxImages; ++i) {
                auto image = std::make_unique<VulkanImage>(device);
                image->initColorAttachment(_extent, kAuxFormats[i]);
                auxWrites.writeStorageImage(_pipeline->descriptorSetHandle(2, frame),
                                             {static_cast<uint32_t>(i), VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                             image->sampleView());
                _auxImages[frame][i] = std::move(image);
            }
        }
        auxWrites.apply();
    }

    device.setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(_pipeline->handle()), "rayquery:primaryRay");

#ifdef R_ENABLE_NRD
    {
        // Stage 1 of the NRD integration: prove the library is linked, its
        // instance comes up, and its resource demands are known. The
        // dispatches themselves arrive with the output split.
        const nrd::LibraryDesc &libraryDesc = *nrd::GetLibraryDesc();
        nrd::DenoiserDesc denoiserDesc {0, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR};
        nrd::InstanceCreationDesc creationDesc {};
        creationDesc.denoisers = &denoiserDesc;
        creationDesc.denoisersNum = 1;
        nrd::Instance *instance = nullptr;
        if (nrd::CreateInstance(creationDesc, instance) == nrd::Result::SUCCESS) {
            _nrdInstance = instance;
            const nrd::InstanceDesc &instanceDesc = *nrd::GetInstanceDesc(*instance);
            info("NRD " + std::to_string(libraryDesc.versionMajor) + "." +
                 std::to_string(libraryDesc.versionMinor) + "." +
                 std::to_string(libraryDesc.versionBuild) + " up: " +
                 std::to_string(instanceDesc.pipelinesNum) + " pipelines, " +
                 std::to_string(instanceDesc.permanentPoolSize) + " permanent + " +
                 std::to_string(instanceDesc.transientPoolSize) + " transient pool textures");
            _nrdDenoiser = std::make_unique<NrdDenoiser>(device, *instance, _extent);
            _nrdDenoiser->init();

            // Nine: output, noise-free, diffuse and specular factors,
            // denoised diffuse and specular, viewZ, then the two raw channels
            // used when a transmitting surface's guide ray misses.
            // Must match the binding list in slang/nrd_composite.slang.
            constexpr uint32_t kCompositeBindingCount = 9;
            std::vector<VulkanPipeline::LayoutBinding> compositeBindings;
            compositeBindings.reserve(kCompositeBindingCount);
            for (uint32_t i = 0; i < kCompositeBindingCount; ++i) {
                compositeBindings.push_back({{i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, 1, VK_SHADER_STAGE_COMPUTE_BIT});
            }
            VulkanPipeline::Config compositeConfig;
            compositeConfig.type = VulkanPipeline::Config::Type::Compute;
            compositeConfig.spirv = _renderer.shaderModule("nrd_composite");
            compositeConfig.computeEntry = "main";
            compositeConfig.descriptorSets = {{_renderer.descriptors().uniformLayout()},
                                              {VK_NULL_HANDLE, std::move(compositeBindings), 2}};
            compositeConfig.pushConstantSize = 3 * sizeof(uint32_t);
            _compositePipeline = std::make_unique<VulkanPipeline>(device);
            _compositePipeline->init(compositeConfig);
        } else {
            warn("NRD instance creation failed; denoising stays unavailable");
        }
    }
#endif
#ifdef R_ENABLE_FSR
    if (_options.ptFsr) {
        // Both at render resolution: NativeAA does not change the size, and the
        // composite/tonemap pair either side of FSR work on the same grid.
        _fsrColor = std::make_unique<VulkanImage>(device);
        _fsrColor->initColorAttachment(_extent, Format::R16G16B16A16Sfloat);
        _fsrOutput = std::make_unique<VulkanImage>(device);
        _fsrOutput->initColorAttachment(_extent, Format::R16G16B16A16Sfloat);

        std::vector<VulkanPipeline::LayoutBinding> tonemapBindings;
        for (uint32_t i = 0; i < 2; ++i) {
            tonemapBindings.push_back({{i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, 1, VK_SHADER_STAGE_COMPUTE_BIT});
        }
        VulkanPipeline::Config tonemapConfig;
        tonemapConfig.type = VulkanPipeline::Config::Type::Compute;
        tonemapConfig.spirv = _renderer.shaderModule("pt_tonemap");
        tonemapConfig.computeEntry = "main";
        tonemapConfig.descriptorSets = {{_renderer.descriptors().uniformLayout()},
                                        {VK_NULL_HANDLE, std::move(tonemapBindings), 2}};
        tonemapConfig.pushConstantSize = 2 * sizeof(uint32_t);
        _tonemapPipeline = std::make_unique<VulkanPipeline>(device);
        _tonemapPipeline->init(tonemapConfig);

        try {
            _fsr = std::make_unique<graphics::FsrUpscaler>(device, _extent);
            _fsr->init();
        } catch (const std::exception &e) {
            // Losing the upscaler must not lose the frame; it costs the
            // anti-aliasing, since FSR is the only temporal resolve left.
            warn(std::string("FSR unavailable, rendering without anti-aliasing: ") + e.what());
            _fsr.reset();
        }
    }
#endif
    _inited = true;
}

void VulkanTracingPipeline::clearFrame(Frame &frame) {
    // The merge and acceleration-structure buffers are capacity-managed. The
    // renderer has waited this in-flight frame's fence before reuse, so a full
    // BLAS/TLAS rebuild may overwrite them, but their allocations survive it.
    frame.traceStats.reset();
}

bool VulkanTracingPipeline::bakeSkyRoom(ICommandBuffer &commandBuffer,
                                         const RayQuerySkyRoom &room) {
    // A failed bake is deliberately sticky for this detected room: the fallback
    // cube is stable, and retrying a known-invalid asset every frame would turn
    // that path into a standing cost. Admission suppresses the shell either way.
    if (_skyCubeRoom == room.identity) {
        return _skyCubeReady;
    }
    _skyCubeRoom = room.identity;
    _skyCubeReady = false;
    if (room.meshes.empty())
        return false;

    auto &device = _renderer.device();
    auto &resources = _renderer.resources();
    auto &ring = _renderer.uniformRing();
    auto &descriptors = _renderer.descriptors();
    if (!_skyCube) {
        _skyCube = std::make_unique<VulkanImage>(device);
        _skyCube->initCubeArrayAttachment({kSkyCubeSize, kSkyCubeSize}, Format::R16G16B16A16Sfloat, 1, 1);
        _skyCube->setSampler(
            resources.samplers().get(getTextureProperties(TextureUsage::ColorBuffer)));
        device.setObjectName(VK_OBJECT_TYPE_IMAGE,
                             reinterpret_cast<uint64_t>(_skyCube->handle()), "Path-traced sky cube");
    }
    bool createDepth = !_skyDepth[0];
    if (createDepth) {
        for (auto &depth : _skyDepth) {
            depth = std::make_unique<VulkanImage>(device);
            depth->initDepthAttachment({kSkyCubeSize, kSkyCubeSize}, Format::D32Sfloat);
        }
    }

    commandBuffer.transitionImage(*_skyCube, ImageLayout::ColorAttachment);
    if (createDepth) {
        for (int face = 0; face < kNumCubeFaces; ++face) {
            commandBuffer.transitionImage(*_skyDepth[face], ImageLayout::DepthAttachment);
        }
    }

    static const glm::vec3 kDirections[kNumCubeFaces] {
        {1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
    };
    static const glm::vec3 kUps[kNumCubeFaces] {
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
    };
    const glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10000.0f);
    const auto uniformSet = _renderer.uniformSet();

    for (int face = 0; face < kNumCubeFaces; ++face) {
        GlobalUniforms globals;
        globals.reset();
        globals.projection = projection;
        globals.projectionInv = glm::inverse(projection);
        globals.view = glm::lookAt(room.origin, room.origin + kDirections[face], kUps[face]);
        globals.viewInv = glm::inverse(globals.view);
        globals.viewProjection = globals.projection * globals.view;
        globals.prevViewProjection = globals.viewProjection;
        globals.cameraPosition = glm::vec4(room.origin, 1.0f);
        std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = ring.push(globals);

        ClearValue colorClear;
        colorClear.color = {0.0f, 0.0f, 0.0f, 1.0f};
        const RenderAttachment color {_skyCube->faceAttachmentView(0, face),
                                      ImageLayout::ColorAttachment,
                                      AttachmentLoad::Clear,
                                      AttachmentStore::Store,
                                      colorClear};
        ClearValue depthClear;
        depthClear.depth = 1.0f;
        depthClear.depthOnly = true;
        const RenderAttachment depth {_skyDepth[face]->sampleView(),
                                      ImageLayout::DepthAttachment,
                                      AttachmentLoad::Clear,
                                      AttachmentStore::DontCare,
                                      depthClear};
        commandBuffer.beginRendering({kSkyCubeSize, kSkyCubeSize}, {color}, &depth, 0, false);
        for (const auto &mesh : room.meshes) {
            const auto &skyMesh = resources.get(*mesh.mesh);
            VulkanPipelineCache::Key key;
            key.module = "sky";
            key.vertexEntry = "skyVertex";
            key.fragmentEntry = "skyFragment";
            key.colorFormats = {toVulkanFormat(_skyCube->pixelFormat())};
            key.depthFormat = toVulkanFormat(Format::D32Sfloat);
            key.depthTest = true;
            key.depthWrite = true;
            key.cull = FaceCullMode::None;
            key.vertexBindings = VulkanMesh::bindingDescriptions(mesh.mesh->vertexLayout());
            key.vertexAttributes = VulkanMesh::attributeDescriptions(mesh.mesh->vertexLayout());
            key.vertexAttributes.erase(
                std::remove_if(key.vertexAttributes.begin(), key.vertexAttributes.end(),
                               [](const auto &attribute) {
                                   return attribute.location != 0 && attribute.location != 2;
                               }),
                key.vertexAttributes.end());
            auto &pipeline = _renderer.pipelines().get(key);

            LocalUniforms locals;
            locals.reset();
            locals.model = mesh.transform;
            locals.modelInv = mesh.transformInv;
            locals.prevModel = mesh.prevTransform;
            locals.uv = mesh.uv;
            offsets[UniformBlockBindingPoints::locals] = ring.push(locals);
            commandBuffer.bindPipeline(toPipeline(pipeline.handle()));
            commandBuffer.bindDescriptorSet(toPipelineLayout(pipeline.layout()),
                                            IDescriptors::kUniformSet,
                                            toDescriptorSet(uniformSet),
                                            offsets.data(), static_cast<uint32_t>(offsets.size()));
            auto textureSet = descriptors.acquireTextureSet(
                _renderer.frameIndex(), {{TextureUnits::mainTex, &resources.get(*mesh.texture)}});
            commandBuffer.bindDescriptorSet(toPipelineLayout(pipeline.layout()),
                                            IDescriptors::kTextureSet,
                                            toDescriptorSet(textureSet), nullptr, 0);
            skyMesh.draw(commandBuffer, resources.zeroBuffer());
        }
        commandBuffer.endRendering();
    }

    commandBuffer.transitionImage(*_skyCube, ImageLayout::ShaderRead);
    _skyCubeReady = true;
    info("Vulkan: baked sky room '" + room.name + "' into a " + std::to_string(kSkyCubeSize) + "px cubemap",
         LogChannel::Graphics);
    return true;
}

void VulkanTracingPipeline::deinit() {
    for (auto &frame : _frames) {
        clearFrame(frame);
    }
#ifdef R_ENABLE_NRD
    _compositePipeline.reset();
    _temporalHistoryValid = false;
    _nrdDenoiser.reset();
    if (_nrdInstance) {
        nrd::DestroyInstance(*static_cast<nrd::Instance *>(_nrdInstance));
        _nrdInstance = nullptr;
    }
#endif
    auto &device = _renderer.device();
    _pipeline.reset();
    for (auto &frame : _auxImages) {
        for (auto &image : frame)
            image.reset();
    }
#ifdef R_ENABLE_FSR
    // Before the device goes: the upscaler owns Vulkan objects of its own, and
    // the two images own VMA allocations that must not outlive the allocator.
    _fsr.reset();
    _fsrColor.reset();
    _fsrOutput.reset();
    _tonemapPipeline.reset();
#endif
    _bindlessTextureCapacity = 0;
    _lastBindlessTextureCount = 0;
    _skyCube.reset();
    for (auto &depth : _skyDepth)
        depth.reset();
    _skyFallbackCube.reset();
    _skyCubeRoom = 0;
    _skyCubeReady = false;
    _lastAuxFrame = -1;
    _inited = false;
}

void VulkanTracingPipeline::clearSkyRoom() {
    _skyCubeRoom = 0;
    _skyCubeReady = false;
}

bool VulkanTracingPipeline::supportsSkyTexture(const Texture &texture) const {
    return VulkanResources::supported(texture.pixelFormat());
}

void VulkanTracingPipeline::restartTemporalHistory() {
    _restartHistoryRequested = true;
#ifdef R_ENABLE_NRD
    _temporalHistoryValid = false;
#endif
}

std::vector<TracingChannel> VulkanTracingPipeline::channels() const {
    if (!_inited || _lastAuxFrame < 0) {
        return {};
    }
    // Order and names follow the aux bindings in tracing/outputs.slang.
    static constexpr const char *kNames[kNumAuxImages] {
        "Traced diffuse", "Traced specular", "Traced normal/roughness",
        "Traced viewZ", "Traced motion", "Traced noise-free", "Traced diffuse factor",
        "Traced device depth", "Traced screen motion", "Traced specular factor",
        "G-buffer diffuse", "G-buffer eye normal", "G-buffer depth", "G-buffer motion"};
    static constexpr const char *kDumpNames[kNumAuxImages] {
        "traced_diffuse", "traced_specular", "traced_normal_roughness",
        "traced_view_z", "traced_motion", "traced_noise_free", "traced_diff_factor",
        "traced_device_depth", "traced_screen_motion", "traced_spec_factor",
        "g_buffer_diffuse", "g_buffer_eye_normal", "g_buffer_depth", "g_buffer_motion"};
    const auto &aux = _auxImages[_lastAuxFrame];
    std::vector<TracingChannel> result;
    for (int i = 0; i < kNumAuxImages; ++i) {
        if (aux[i]) {
            result.push_back({kNames[i], kDumpNames[i], aux[i].get()});
        }
    }
#ifdef R_ENABLE_NRD
    // What NRD made of the two radiance channels. Comparing these against the
    // raw pair above is the difference between "the tracer is noisy" and "the
    // denoiser is not removing it".
    if (_nrdDenoiser) {
        result.push_back({"Denoised diffuse", "denoised_diffuse", &_nrdDenoiser->denoisedDiffuse()});
        result.push_back({"Denoised specular", "denoised_specular", &_nrdDenoiser->denoisedSpecular()});
    }
#endif
    return result;
}

TracingStats VulkanTracingPipeline::render(const TracingPipelineInput &input) {
    auto &commandBuffer = input.commandBuffer;
    auto &output = input.output;
    const auto &view = input.view;
    const auto &projection = input.projection;
    const auto &jitter = input.jitter;
    const auto &scene = input.scene;
    const auto globalsOffset = input.globalsOffset;
    const auto frameNumber = input.frameNumber;
    const auto skyBaked = input.skyBaked;
    R_PROFILE_ZONE("VulkanTracingPipeline::render");
    const auto nativeCommandBuffer = toVulkanCommandBuffer(commandBuffer).handle();
    const int frameIndex = input.frameIndex;
    // A valid traced frame can contain no merged geometry. That path clears
    // the output and returns below, but its auxiliary images are still useful
    // diagnostics (and must not disappear from --dumptargets just because the
    // scene is empty). Record the selected double-buffer slot before that
    // early return, rather than only after the trace dispatch.
    _lastAuxFrame = frameIndex;
    auto &frame = _frames[frameIndex];
    TracingStats previousStats;
    // The renderer waited this in-flight frame's fence before calling us, so
    // its previous GPU-written counters are now safe to inspect.
    if (frame.traceStats) {
        frame.traceStats->invalidateMapped();
        const auto *stats = static_cast<const TraceStats *>(frame.traceStats->mapped());
        previousStats = {stats->secondaryRays, stats->secondaryMisses,
                         stats->survivingLights, stats->primaryHits,
                         stats->shadowRays};
    }
    clearFrame(frame);
    if (!scene.vertices.buffer) {
        previousStats.bindlessTextures = _lastBindlessTextureCount;
        return previousStats;
    }
    auto &device = _renderer.device();
    frame.traceStats = _renderer.makeBuffer();
    frame.traceStats->initHostVisibleReadback(sizeof(TraceStats));
    std::memset(frame.traceStats->mapped(), 0, sizeof(TraceStats));

    const auto set = _pipeline->descriptorSet(1, _renderer.frameIndex());
    DescriptorWriteBuilder writes(device.handle());
    writes.writeStorageImage(toDescriptorSet(set), {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                             output.sampleView());
    writes.writeAccelerationStructure(
        set, {1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}, input.structure.handle());
    writes.writeBuffer(set, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                       {nativeBuffer(scene.materials.buffer->rhiHandle()), 0, scene.materials.size});
    writes.writeBuffer(set, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                       {nativeBuffer(frame.traceStats->rhiHandle()), 0, frame.traceStats->size()});
    writes.writeBuffer(set, {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                       {nativeBuffer(scene.vertices.buffer->rhiHandle()), scene.vertices.offset, scene.vertices.size});
    writes.writeBuffer(set, {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                       {nativeBuffer(scene.vertices.buffer->rhiHandle()), scene.indices.offset, scene.indices.size});
    writes.writeBuffer(set, {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                       {nativeBuffer(scene.materialIds.buffer->rhiHandle()), scene.materialIds.offset, scene.materialIds.size});
    writes.apply();
    const VulkanImage &skyImage = skyBaked ? *_skyCube : *_skyFallbackCube;
    DescriptorWriteBuilder skyWrite(device.handle());
    skyWrite.writeSampledImage(toDescriptorSet(set), {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                               skyImage.sampleSampler(),
                               skyBaked ? toImageView(_skyCube->cubeView(0)) : _skyFallbackCube->sampleView());
    skyWrite.apply();
    // Texture ids are assigned by the resource cache at upload time. The set is
    // update-after-bind and partially-bound so new assets can take a slot
    // without rebuilding it or populating unrelated descriptors.
    const auto uploadedTextures = _renderer.resources().uploadedTextures();
    DescriptorWriteBuilder textureWrites(device.handle());
    for (const auto &[id, texture] : uploadedTextures) {
        if (id >= _bindlessTextureCapacity) {
            throw std::runtime_error("Vulkan: ray-query bindless texture array exhausted");
        }
        textureWrites.writeSampledImage(toDescriptorSet(set),
                                        {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                                        texture->sampleSampler(), texture->sampleView(), id);
    }
    textureWrites.apply();
    const auto uploadedTextureArrays = _renderer.resources().uploadedTextureArrays();
    DescriptorWriteBuilder textureArrayWrites(device.handle());
    for (const auto &[id, texture] : uploadedTextureArrays) {
        if (id >= _bindlessTextureCapacity) {
            throw std::runtime_error("Vulkan: ray-query bindless texture array exhausted");
        }
        textureArrayWrites.writeSampledImage(toDescriptorSet(set),
                                             {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                                             texture->sampleSampler(), texture->sampleView(), id);
    }
    textureArrayWrites.apply();
    _lastBindlessTextureCount = static_cast<uint32_t>(uploadedTextures.size());
    {
        // Every aux image lives in GENERAL. The tracked transition is a no-op
        // after the first traced frame while retaining the one dependency.
        for (int frameIndex = 0; frameIndex < 2; ++frameIndex) {
            for (int i = 0; i < kNumAuxImages; ++i) {
                commandBuffer.transitionImage(*_auxImages[frameIndex][i], ImageLayout::General);
            }
        }
    }
    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[0] = globalsOffset;
    auto uniformSet = _renderer.uniformSet();
    commandBuffer.bindRayTracingPipeline(_pipeline->pipeline());
    commandBuffer.bindRayTracingDescriptorSet(_pipeline->pipelineLayout(), 0,
                                              toDescriptorSet(uniformSet), offsets.data(),
                                              static_cast<uint32_t>(offsets.size()));
    commandBuffer.bindRayTracingDescriptorSet(_pipeline->pipelineLayout(), 1,
                                              toDescriptorSet(set), nullptr, 0);
    const auto auxSet = _pipeline->descriptorSetHandle(2, _renderer.frameIndex());
    commandBuffer.bindRayTracingDescriptorSet(_pipeline->pipelineLayout(), 2, auxSet, nullptr, 0);
    // Clamped rather than trusted: the option is user-editable in reone.cfg
    // and a zero would divide the accumulated radiance by zero.
    TracePushConstants constants {frameNumber,
                                  static_cast<uint32_t>(std::max(1, _options.pathTracingSamples)),
                                  std::max(0.0f, _options.ptSkyIntensity),
                                  std::max(0.0f, _options.ptEmissiveIntensity),
                                  std::max(0.0f, _options.ptLightmapIntensity),
                                  std::max(0.0f, _options.ptDirectIntensity),
                                  std::max(0.0001f, _options.ptRayOffset),
                                  std::max(0.0f, _options.ptSunIntensity),
                                  (_options.ptTraceStats ? 1u : 0u) |
                                      (static_cast<uint32_t>(std::clamp(_options.ptDebugView, 0, 12)) << 4) |
                                      (static_cast<uint32_t>(std::clamp(_options.ptTonemap, 0, 1)) << 8),
                                  static_cast<uint32_t>(std::clamp(_options.ptBounces, 1, 8)),
                                  std::clamp(_options.ptPointEmitterRatio, 0.01f, 0.5f),
                                  glm::radians(std::clamp(_options.ptSunAngularSize, 0.05f, 10.0f)),
                                  std::max(0.01f, _options.ptExposure),
                                  0,
                                  scene.opaqueTriangleCount,
                                  skyBaked ? 1u : 0u};
    commandBuffer.pushRayTracingConstants(_pipeline->pipelineLayout(), &constants, sizeof(constants));
    {
        R_PROFILE_ZONE("RayQuery::dispatch record");
        commandBuffer.traceRays(
            _pipeline->pipeline(), input.structure,
            {static_cast<uint32_t>(_extent.x), static_cast<uint32_t>(_extent.y)});
    }
#ifdef R_ENABLE_NRD
    if (_nrdDenoiser) {
        // The trace pass's storage writes feed NRD's sampled reads.
        commandBuffer.publishTraceOutputForDenoising();
        const auto &aux = _auxImages[_renderer.frameIndex()];
        NrdDenoiser::Inputs inputs;
        inputs.diffRadianceHitDist = toVulkanImageView(aux[0]->sampleView());
        inputs.specRadianceHitDist = toVulkanImageView(aux[1]->sampleView());
        inputs.normalRoughness = toVulkanImageView(aux[2]->sampleView());
        inputs.viewZ = toVulkanImageView(aux[3]->sampleView());
        inputs.motion = toVulkanImageView(aux[4]->sampleView());
        // The projection arrives carrying the TAA jitter (applied as a clip
        // translate); NRD is owed the unjittered matrix and the sub-pixel
        // offset separately, the latter in pixels with UV-down y.
        glm::mat4 unjitteredProjection =
            glm::translate(glm::vec3(-jitter.x, -jitter.y, 0.0f)) * projection;
        glm::vec2 jitterPixels {jitter.x * 0.5f * static_cast<float>(_extent.x),
                                -jitter.y * 0.5f * static_cast<float>(_extent.y)};
        NrdDenoiser::Tuning tuning;
        tuning.maxAccumulatedFrames = _options.ptNrdMaxAccumulatedFrames;
        tuning.maxFastAccumulatedFrames = _options.ptNrdMaxFastAccumulatedFrames;
        tuning.maxStabilizedFrames = _options.ptNrdMaxStabilizedFrames;
        tuning.historyFixFrames = _options.ptNrdHistoryFixFrames;
        tuning.diffusePrepassBlurRadius = _options.ptNrdDiffusePrepassBlurRadius;
        tuning.specularPrepassBlurRadius = _options.ptNrdSpecularPrepassBlurRadius;
        tuning.minBlurRadius = _options.ptNrdMinBlurRadius;
        tuning.maxBlurRadius = _options.ptNrdMaxBlurRadius;
        tuning.lobeAngleFraction = _options.ptNrdLobeAngleFraction;
        tuning.roughnessFraction = _options.ptNrdRoughnessFraction;
        tuning.planeDistanceSensitivity = _options.ptNrdPlaneDistanceSensitivity;
        tuning.disocclusionThreshold = _options.ptNrdDisocclusionThreshold;
        tuning.antiFirefly = _options.ptNrdAntiFirefly;
        const bool restartHistory = frameNumber == 0 || _restartHistoryRequested;
        _restartHistoryRequested = false;
        _nrdDenoiser->denoise(nativeCommandBuffer, _renderer.frameIndex(), inputs, tuning, view, unjitteredProjection,
                               jitterPixels, frameNumber, restartHistory);
        if (_options.ptDenoise && _options.ptDebugView == 0) {
            // The noise-free history resets whenever NRD's would: first
            // frame, or a teleport-sized camera jump.
            const auto cameraPosition = glm::vec3(glm::inverse(view)[3]);
            if (frameNumber == 0 ||
                glm::distance(cameraPosition, _prevCameraPosition) > 20.0f) {
                _temporalHistoryValid = false;
            }
            _prevCameraPosition = cameraPosition;
            const bool temporalReset = !_temporalHistoryValid;

            // The assembly from denoised channels, overwriting the trace
            // kernel's own write. Debug views keep the kernel's output.
            // History ping-pong: the frame at index i reads what the previous
            // frame (index 1-i) wrote into slot i, and writes slot 1-i.
            const auto frameIndex = _renderer.frameIndex();
            const auto compositeSet = _compositePipeline->descriptorSet(1, frameIndex);
            // With FSR the composite hands off linear HDR to the upscaler
            // instead of writing the finished frame; the display transform
            // happens after, in pt_tonemap.
            bool fsrActive = false;
#ifdef R_ENABLE_FSR
            fsrActive = _fsr && _fsr->inited();
#endif
            ImageView compositeTarget = output.sampleView();
#ifdef R_ENABLE_FSR
            if (fsrActive) {
                commandBuffer.transitionImage(*_fsrColor, ImageLayout::General);
                commandBuffer.transitionImage(*_fsrOutput, ImageLayout::General);
                compositeTarget = _fsrColor->sampleView();
            }
#endif
            // Motion is not among them: it existed only for the removed TAA's
            // reprojection. NRD still consumes it directly.
            constexpr uint32_t kCompositeBindings = 9;
            const std::array<ImageView, kCompositeBindings> compositeImages {{
                compositeTarget,
                aux[5]->sampleView(),
                aux[6]->sampleView(),
                aux[9]->sampleView(),
                _nrdDenoiser->denoisedDiffuse().sampleView(),
                _nrdDenoiser->denoisedSpecular().sampleView(),
                aux[3]->sampleView(),
                aux[0]->sampleView(),
                aux[1]->sampleView(),
            }};
            DescriptorWriteBuilder compositeWrites(device.handle());
            for (uint32_t i = 0; i < kCompositeBindings; ++i) {
                compositeWrites.writeStorageImage(toDescriptorSet(compositeSet),
                                                  {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                                  compositeImages[i]);
            }
            compositeWrites.apply();
            struct CompositePush {
                uint32_t tonemap;
                float exposure;
                uint32_t linearOutput;
            } compositePush {static_cast<uint32_t>(std::clamp(_options.ptTonemap, 0, 1)),
                             std::max(0.01f, _options.ptExposure),
                             fsrActive ? 1u : 0u};
            commandBuffer.bindComputePipeline(_compositePipeline->pipeline());
            commandBuffer.bindComputeDescriptorSet(_compositePipeline->pipelineLayout(), 0,
                                                   toDescriptorSet(uniformSet), offsets.data(),
                                                   static_cast<uint32_t>(offsets.size()));
            commandBuffer.bindComputeDescriptorSet(_compositePipeline->pipelineLayout(), 1,
                                                   toDescriptorSet(compositeSet), nullptr, 0);
            commandBuffer.pushComputeConstants(_compositePipeline->pipelineLayout(),
                                               &compositePush, sizeof(compositePush));
            commandBuffer.dispatch({static_cast<uint32_t>((_extent.x + 7) / 8),
                                    static_cast<uint32_t>((_extent.y + 7) / 8), 1});
            _temporalHistoryValid = true;
#ifdef R_ENABLE_FSR
            if (fsrActive) {
                // Composite writes, FSR reads. FSR's backend barriers its own
                // internal resources but not ours, so the handoff is ours.
                commandBuffer.publishCompositeForUpscaling();

                graphics::FsrUpscaler::Inputs fsrInputs;
                fsrInputs.color = _fsrColor.get();
                fsrInputs.depth = aux[7].get();
                fsrInputs.motion = aux[8].get();
                fsrInputs.output = _fsrOutput.get();
                // The same sub-pixel offset NRD is given: FSR's jitter
                // convention and ours already agree, both being a pixel-space
                // Halton(2,3) with y negated for the UV-down axis.
                const float verticalFov =
                    2.0f * std::atan(1.0f / std::max(1e-4f, projection[1][1]));
                // Read the planes back out of the native Vulkan projection
                // rather than plumbing them down: unprojecting both ends of
                // the depth range avoids depending on its coefficient layout.
                const glm::mat4 projectionInv = glm::inverse(projection);
                const glm::vec4 nearH = projectionInv * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                const glm::vec4 farH = projectionInv * glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
                const float cameraNear = std::abs(nearH.z / nearH.w);
                const float cameraFar = std::abs(farH.z / farH.w);
                _fsr->dispatch(nativeCommandBuffer, fsrInputs, jitterPixels, 1.0f / 60.0f,
                               cameraNear, cameraFar, verticalFov, _options.ptFsrSharpness,
                                frameNumber == 0 || temporalReset);

                // The backend deliberately leaves its inputs ready for sampled
                // reads. The trace and composite passes write these images as
                // storage images again on the next frame, so restore GENERAL.
                commandBuffer.restoreUpscalerInputsForNextFrame(*_fsrColor, *aux[7], *aux[8]);
                commandBuffer.publishUpscaledFrameForTonemapping();

                const auto tonemapSet = _tonemapPipeline->descriptorSet(1, frameIndex);
                const std::array<ImageView, 2> tonemapImages {{
                    output.sampleView(), _fsrOutput->sampleView(),
                }};
                DescriptorWriteBuilder tonemapWrites(device.handle());
                for (uint32_t i = 0; i < 2; ++i) {
                    tonemapWrites.writeStorageImage(toDescriptorSet(tonemapSet),
                                                    {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                                    tonemapImages[i]);
                }
                tonemapWrites.apply();
                struct TonemapPush {
                    uint32_t tonemap;
                    float exposure;
                } tonemapPush {static_cast<uint32_t>(std::clamp(_options.ptTonemap, 0, 1)),
                               std::max(0.01f, _options.ptExposure)};
                commandBuffer.bindComputePipeline(_tonemapPipeline->pipeline());
                commandBuffer.bindComputeDescriptorSet(_tonemapPipeline->pipelineLayout(), 0,
                                                       toDescriptorSet(uniformSet), offsets.data(),
                                                       static_cast<uint32_t>(offsets.size()));
                commandBuffer.bindComputeDescriptorSet(_tonemapPipeline->pipelineLayout(), 1,
                                                       toDescriptorSet(tonemapSet), nullptr, 0);
                commandBuffer.pushComputeConstants(_tonemapPipeline->pipelineLayout(),
                                                   &tonemapPush, sizeof(tonemapPush));
                commandBuffer.dispatch({static_cast<uint32_t>((_extent.x + 7) / 8),
                                        static_cast<uint32_t>((_extent.y + 7) / 8), 1});
            }
#endif
        }
    }
#endif
    previousStats.bindlessTextures = _lastBindlessTextureCount;
    return previousStats;
}

std::unique_ptr<ITracingStructure> VulkanTracingPipeline::makeTracingStructure() {
    return graphics::makeTracingStructure(_renderer.device());
}
} // namespace reone::graphics
