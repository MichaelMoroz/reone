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

#include "reone/graphics/rendering/scenepipeline.h"

#include "reone/system/profiler.h"

#include "reone/graphics/dxtutil.h"
#include "reone/graphics/npyutil.h"
#include "reone/graphics/font.h"
#include "reone/graphics/options.h"
#include "reone/graphics/textureregistry.h"
#include "reone/graphics/textureutil.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/rendering/pbrtextures.h"
#include "reone/graphics/rhi/descriptors.h"
#include "reone/graphics/rhi/pipelinecache.h"
#include "reone/graphics/rhi/resources.h"
#include "reone/graphics/rhi/uniformring.h"
#include "reone/system/logutil.h"

#include <algorithm>
#include <cmath>
#include <string_view>

// glm::translate, for undoing the projection's jitter translate before handing
// the matrix to a resolver that takes the sub-pixel offset separately.
#include <glm/gtc/matrix_transform.hpp>

using namespace reone::graphics;

namespace reone {

namespace graphics {

static constexpr char kPostProcessModule[] = "postprocess";

struct MegaDrawPushConstants {
    uint32_t triangleBase;
    uint32_t materialGated;
};

/** The blended pass's third word: kResolveFlag* bits the fragment must honour. */
struct BlendedPushConstants {
    uint32_t triangleBase;
    uint32_t materialGated;
    uint32_t flags;
    /** The shadow pass's word, unread here; keeps the fog fields at the offsets
        the shared PushConstants block in scene_draw.slang declares them. */
    uint32_t shadowSlot;
    /** Mirrors FogPushConstants: the blended pass fogs its own surfaces. */
    float fogDensity;
    float fogFalloff;
    float fogPlaneZ;
};

/** The shadow pass's third word: which categories may write the map. */
struct ShadowPushConstants {
    uint32_t triangleBase;
    uint32_t materialGated;
    uint32_t casterCategories;
    /** Which caster is being rendered; the vertex stage reads it. */
    uint32_t shadowSlot;
};

/** Mirrors PostProcessPushConstants in postprocess.slang. */
struct PostProcessPushConstants {
    uint32_t grade;
    uint32_t displayReferred;
    float exposure;
    uint32_t tonemap;
};

/** Mirrors ResolvePushConstants in pbr_channels.slang and retro_resolve.slang. */
struct ResolvePushConstants {
    uint32_t flags;
    float thinTransmission;
    float albedoGamma;
    /** Point-light falloff is evaluated no closer than this share of the authored radius. */
    float lightDistanceClamp;
    float lightmapIntensity;
    /** Emitter size as a fraction of influence radius; PBR's sphere lights. */
    float emitterRadiusRatio;
    float directIntensity;
    float sunIntensity;
    /** The baked sky cube's grade, shared with the tracer. */
    float skyIntensity;
    float skyGamma;
    /** Keeps the block at the cached push size. */
    float pad;
};

/** Mirrors CoveragePushConstants in postprocess.slang. */
struct CoveragePushConstants {
    uint32_t skyAvailable;
};

/** Mirrors DebugViewPushConstants in debug_view.slang. */
struct DebugViewPushConstants {
    /** The channel, plus the channels-absent flag bit (retro has no channel
        images, so its radiance views show the card); see debug_view.slang. */
    uint32_t view;
    // The same display transform the tracer's channel views apply
    // (path_trace.slang finishPixel), so the two renderers' channels land in
    // ONE colour space. The debug pass runs after post-processing would have
    // and overwrites its output, so it has to encode for itself - a raw write
    // here measured as a factor-of-pi-shaped error against the tracer before
    // anyone thought to check the encoding.
    float exposure;
    uint32_t tonemap;
};

/** Mirrors FogPushConstants in postprocess.slang. */
struct FogPushConstants {
    float density;
    float falloff;
    float planeZ;
};
static_assert(sizeof(FogPushConstants) <= kCachedPipelinePushConstantSize);

/** Mirrors DebugOverlayPushConstants in debug_overlay.slang. */
struct DebugOverlayPushConstants {
    glm::vec4 color;
    /** Target extent in pixels: both stages work in pixels and convert back. */
    glm::vec2 resolution;
    /** Half a line's width, in pixels; the label entry points ignore it. */
    float halfWidth;
    /** Opacity of whatever part of this primitive lies behind geometry. */
    float occludedScale;
    /** World point a label hangs from; the box entry points ignore it. */
    glm::vec3 anchor;
};

static_assert(sizeof(MegaDrawPushConstants) <= kCachedPipelinePushConstantSize);
static_assert(sizeof(BlendedPushConstants) <= kCachedPipelinePushConstantSize);
static_assert(sizeof(ShadowPushConstants) <= kCachedPipelinePushConstantSize);
static_assert(sizeof(PostProcessPushConstants) <= kCachedPipelinePushConstantSize);
static_assert(sizeof(ResolvePushConstants) == kCachedPipelinePushConstantSize);
static_assert(sizeof(CoveragePushConstants) <= kCachedPipelinePushConstantSize);
static_assert(sizeof(DebugViewPushConstants) <= kCachedPipelinePushConstantSize);

/** Mirrors kViewChannelsAbsent in debug_view.slang: no mode filled the shared
    channels this frame, so the radiance views must paint the card. */
static constexpr uint32_t kDebugViewChannelsAbsent = 1u << 8;
static_assert(sizeof(DebugOverlayPushConstants) <= kCachedPipelinePushConstantSize);

/**
 * The shadow pass's pipeline key. Only the two entry points and the view mask
 * vary - between a directional caster and a point one, and between merged
 * meshes and grass cards - so the rest is described once here rather than
 * rebuilt per draw inside the caster loop, where its four std::strings and a
 * vector cost a handful of allocations every frame per caster.
 *
 * D32_SFLOAT constant bias is expressed in representable depth increments; the
 * slope term supplies the useful offset on curved surfaces that approach
 * parallel to the light. Both carry more than they used to because nothing is
 * culled any more: rendering back faces alone put the stored depth a wall's
 * thickness behind the lit surface, a free bias - but only for geometry that
 * HAS a back face. Odyssey's exterior shells are single-sided, so from the
 * sun's side they wrote nothing and light poured into the rooms behind them.
 * With both faces written a lit surface finds its own depth in the map and
 * needs a real offset. The receiver-side normal offset in lib/shadow.slang
 * remains the primary defence; these are the dials to turn if acne or
 * peter-panning shows up. Never cull: a single-sided wall has to occlude from
 * whichever side the light is on, and a cutout fence or leaf card likewise.
 */
PipelineKey shadowPipelineKey(const char *vertexEntry, const char *fragmentEntry,
                              uint32_t viewMask) {
    PipelineKey key;
    key.module = "scene_draw";
    key.vertexEntry = vertexEntry;
    key.fragmentEntry = fragmentEntry;
    key.depthFormat = Format::D32Sfloat;
    key.viewMask = viewMask;
    key.depthTest = true;
    key.depthWrite = true;
    key.depthBias = true;
    key.depthBiasConstantFactor = 2.0f;
    key.depthBiasSlopeFactor = 2.0f;
    key.cull = FaceCullMode::None;
    return key;
}

/**
 * ln(100): the optical depth at which transmittance is a hundredth.
 *
 * Used twice, for the two ends of the fog's shape - horizontally it sets the
 * density so a ground-level ray is ~99% fogged at the area's authored far
 * distance, and vertically it sets the falloff so density is a hundredth of
 * its ground value at the gradient height.
 */
static constexpr float kFogOpticalDepthAtFar = 4.60517f;
/** ln(2): the optical depth at which transmittance is one half - where the
    exponential is made to agree with the linear ramp it stands in for. */
static constexpr float kFogOpticalDepthAtMidpoint = 0.693147f;

/** Half-width of a debug-overlay line, in pixels. */
static constexpr float kOverlayLineHalfWidth = 1.25f;
/** Opacity of the part of an overlay line that lies behind geometry. */
static constexpr float kOverlayOccludedLine = 0.28f;
/**
 * The same, for a label. Higher than a line's on purpose: a one-pixel stroke
 * still reads as a line at 28%, while a glyph is mostly edge and does not.
 */
static constexpr float kOverlayOccludedLabel = 0.55f;

/** A sky bake is available this frame; without it the resolves write black. */
static constexpr uint32_t kResolveFlagSky = 1u;
/** Screen-space occlusion; read by the PBR resolve alone. */
static constexpr uint32_t kResolveFlagSSAO = 2u;
/** The target is composited by GUI and uncovered pixels must remain transparent. */
static constexpr uint32_t kResolveFlagTransparentOutput = 4u;
/**
 * The scene target holds DISPLAY-REFERRED colour for the rest of the chain.
 *
 * Retro only, and not an optimisation - a fidelity requirement. Retro's shading
 * is the original's gamma-space arithmetic and its display transform is the
 * identity, so encoding to linear on the way out of the resolve and decoding
 * again in post is a round trip that cancels. It cancels for the ENDPOINTS. It
 * does not cancel for anything that reads or writes the target in between:
 * an alpha blend, a bloom threshold and a blur all give different answers in
 * the two spaces, and the original performed every one of them in display
 * space, on an 8-bit framebuffer with no sRGB.
 *
 * Measured on danm14aa once the classification change let foliage blend at all:
 * compositing the same layer in display space instead of linear accounts for
 * about two thirds of the difference from the reference build.
 *
 * PBR and path tracing keep the linear chain, deliberately. There the round
 * trip does NOT cancel - exposure and a tone curve sit inside it - and linear
 * compositing is the more correct of the two, which is the exception the
 * all-modes rule allows for.
 */
static constexpr uint32_t kResolveFlagDisplayReferred = 8u;
/** Mirrors kResolveFlagParityDirect in pbr_channels.slang. */
static constexpr uint32_t kResolveFlagParityDirect = 16u;
/** The area authored fog and the player's switch is on; the blended pass fogs
    its own surfaces on it. Mirrors kMegaFlagFog in scene_draw.slang. */
static constexpr uint32_t kResolveFlagFog = 32u;
/** Replace GUI-model shading with coverage, cutout threshold, and class in RGB. */

/** Both resolve dispatches, and their shader, agree on this tile. */
static constexpr uint32_t kResolveGroupSize = 8;

/**
 * A cosine-weighted hemisphere kernel, packed toward the origin.
 *
 * Built from a Hammersley sequence rather than from random numbers so that it
 * is the same kernel on every machine and in every run - the point of an
 * occlusion term is that it describes the geometry, and a capture that differed
 * between two builds for kernel reasons would be unreadable. The quadratic ramp
 * on the radius is the usual one: it concentrates samples near the point being
 * shaded, which is where occlusion actually varies.
 */
static std::array<glm::vec4, kNumSSAOSamples> buildSSAOKernel() {
    std::array<glm::vec4, kNumSSAOSamples> kernel {};
    for (int i = 0; i < kNumSSAOSamples; ++i) {
        // Radical inverse base 2, the second Hammersley coordinate.
        uint32_t bits = static_cast<uint32_t>(i);
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        const float radical = static_cast<float>(bits) * 2.3283064365386963e-10f;
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(kNumSSAOSamples);

        const float phi = 2.0f * 3.14159265358979323846f * radical;
        // Cosine-weighted: z is the square root of a uniform, so the density
        // follows the diffuse lobe the term stands in for.
        const float cosTheta = std::sqrt(1.0f - u);
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        glm::vec3 sample {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};

        float scale = static_cast<float>(i) / static_cast<float>(kNumSSAOSamples);
        scale = 0.1f + 0.9f * scale * scale;
        kernel[i] = glm::vec4(sample * scale, 0.0f);
    }
    return kernel;
}

ScenePipeline::ScenePipeline(glm::ivec2 targetSize,
                                         GraphicsOptions &options,
                                         IRenderer &renderer,
                                         Uniforms &uniforms,
                                         IMeshRegistry &meshRegistry,
                                         TextureRegistry &textureRegistry,
                                         bool primaryRayMode) :
    _targetSize(std::move(targetSize)),
    _options(options),
    _renderer(renderer),
    _uniforms(uniforms),
    _meshRegistry(meshRegistry),
    _textureRegistry(textureRegistry),
    _primaryRayMode(primaryRayMode) {
}

ScenePipeline::~ScenePipeline() {
    deinit();
}

static void transitionGBuffer(ICommandBuffer &cmd, GBuffer &gbuffer,
                              ImageLayout layout) {
    cmd.transitionImages(gbuffer.colorImages(), layout);
}

void ScenePipeline::init() {
    if (_inited) {
        return;
    }
    _renderSize = renderExtentFor(_options, _targetSize);

    _gbuffer = std::make_unique<GBuffer>(_renderer);
    _gbuffer->init(_renderSize);

    // Float in every mode, because every mode's scene output is now linear
    // scene-referred colour and the display transform is the post-process pass
    // for all of them. An 8-bit unorm here would clamp radiance above one
    // before the tonemap ever saw it, and would quantise retro's gamma round
    // trip - retro computes in the original's display space and converts back
    // to linear on the way out, which only cancels exactly if the intermediate
    // can hold the exponentiated value. The swapchain's format is where the
    // frame becomes bytes, one blit later, and that has not moved.
    const Format outputFormat = Format::R16G16B16A16Sfloat;
    _output = _renderer.resources().makeImage();
    _output->initColorAttachment(_renderSize, outputFormat);
    _tailColor = _renderer.resources().makeImage();
    _tailColor->initColorAttachment(_renderSize, outputFormat);
    _bloomA = _renderer.resources().makeImage();
    _bloomA->initColorAttachment(_renderSize, outputFormat);
    _bloomB = _renderer.resources().makeImage();
    _bloomB->initColorAttachment(_renderSize, outputFormat);
    // Allocated only when the upscale actually changes resolution. At
    // NativeAA the chain never swaps and these would be dead memory the size
    // of the frame.
    if (_renderSize != _targetSize) {
        _displayColor = _renderer.resources().makeImage();
        _displayColor->initColorAttachment(_targetSize, outputFormat);
        _displayTail = _renderer.resources().makeImage();
        _displayTail->initColorAttachment(_targetSize, outputFormat);
    }

    auto colorSampler = _renderer.resources().sampler(
        getTextureProperties(TextureUsage::ColorBuffer));
    auto depthSampler = _renderer.resources().sampler(
        getTextureProperties(TextureUsage::DepthBuffer));
    // An integer target admits no filtering at all, and a triangle id is a name
    // rather than a quantity: interpolating two of them would name a third
    // triangle that covers neither pixel.
    auto triangleIdProperties = getTextureProperties(TextureUsage::ColorBuffer);
    triangleIdProperties.minFilter = Texture::Filtering::Nearest;
    triangleIdProperties.magFilter = Texture::Filtering::Nearest;
    auto triangleIdSampler = _renderer.resources().sampler(triangleIdProperties);
    _output->setSampler(colorSampler);
    _tailColor->setSampler(colorSampler);
    if (_displayColor) {
        _displayColor->setSampler(colorSampler);
        _displayTail->setSampler(colorSampler);
    }
    _gbuffer->setSamplers(colorSampler, depthSampler, triangleIdSampler);

    // The tracer resolves opaque shadows with rays, but its forward transparent
    // tail is rasterized and samples these maps through the same texture set as
    // PBR. Keep both resources valid in every mode that can record that tail.
    {
        const auto budget = shadowBudgetFor(_options);
        _dirShadowSlots = std::max(1, budget.directional);
        _pointShadowSlots = std::max(1, budget.point);
        // One slot per caster the mode may hold. Sized from the same policy the
        // scene selects against, so a caster can never exist without a map.
        glm::ivec2 shadowSize {_options.shadowResolution, _options.shadowResolution};
        glm::ivec2 pointSize {_options.pointShadowResolution, _options.pointShadowResolution};
        _dirShadows = _renderer.resources().makeImage();
        _dirShadows->initLayeredDepthAttachment(
            shadowSize, Format::D32Sfloat,
            std::max(1, budget.directional) * kNumShadowCascades, false);
        _pointShadows = _renderer.resources().makeImage();
        _pointShadows->initLayeredDepthAttachment(
            pointSize, Format::D32Sfloat,
            std::max(1, budget.point) * kNumCubeFaces, true);
        // A comparison sampler, and linear where the G-buffer's depth sampler is
        // nearest: the filtering unit compares the four texels around the lookup
        // against the receiver's depth and returns the fraction that passed, so
        // one tap is already a 2x2 percentage-closer filter. Nearest would make
        // it compare one texel and hand back 0 or 1, which is the hand-rolled
        // test with extra steps. The white border still reads as far, so a
        // lookup off the edge of a cascade stays lit.
        auto shadowProperties = getTextureProperties(TextureUsage::DepthBuffer);
        shadowProperties.minFilter = Texture::Filtering::Linear;
        shadowProperties.magFilter = Texture::Filtering::Linear;
        shadowProperties.compare = true;
        auto shadowSampler = _renderer.resources().sampler(shadowProperties);
        _dirShadows->setSampler(shadowSampler);
        _pointShadows->setSampler(shadowSampler);
        // The same cube image binds a second time under a PLAIN sampler, for
        // the penumbra's blocker search: a comparison sampler answers "is the
        // receiver behind" and cannot return the blocker's actual distance.
        // Nearest, because averaging two stored depths across an occluder edge
        // invents a blocker at a distance where nothing stands.
        auto rawShadowProperties = shadowProperties;
        rawShadowProperties.compare = false;
        rawShadowProperties.minFilter = Texture::Filtering::Nearest;
        rawShadowProperties.magFilter = Texture::Filtering::Nearest;
        _pointShadowRawSampler = _renderer.resources().sampler(rawShadowProperties);

        // Both resolve sets always bind both sampler shapes. Clear each target to
        // the far plane once so the inactive light kind is a valid no-shadow map.
        // Every slot cleared to the far plane once, not just the first: a slot
        // that never fills is still bound and still sampled, and an uncleared
        // one reads as an occluder sitting on top of the scene. The two kinds
        // are cleared a slot at a time because a view mask cannot span more
        // views than multiview allows.
        _renderer.immediateSubmit([this, shadowSize, pointSize, budget](ICommandBuffer &cmd) {
            auto clearSlots = [&](IImage &image, glm::ivec2 extent, int slots, int layersPerSlot) {
                cmd.transitionImage(image, ImageLayout::DepthAttachment);
                for (int slot = 0; slot < std::max(1, slots); ++slot) {
                    RenderAttachment depth {
                        image.layerAttachmentView(slot * layersPerSlot, layersPerSlot),
                        ImageLayout::DepthAttachment,
                        AttachmentLoad::Clear, AttachmentStore::Store};
                    depth.clear.depthOnly = true;
                    cmd.beginRendering(extent, {}, &depth, (1u << layersPerSlot) - 1u, false);
                    cmd.endRendering();
                }
                cmd.transitionImage(image, ImageLayout::DepthRead);
            };
            clearSlots(*_dirShadows, shadowSize, budget.directional, kNumShadowCascades);
            clearSlots(*_pointShadows, pointSize, budget.point, kNumCubeFaces);
        });

        IDescriptors &descriptors = _renderer.descriptors();
        _retroResolveSet = descriptors.createPersistentTextureSet(
            {{1, &_gbuffer->color(GBufferAttachment::Diffuse)},
             {2, &_gbuffer->color(GBufferAttachment::EyeNormal)},
             {3, &_gbuffer->color(GBufferAttachment::Lightmap)},
             {4, &_gbuffer->color(GBufferAttachment::SelfIllum)},
             {5, &_gbuffer->depth()},
             {15, _dirShadows.get()},
             {17, &_renderer.pbrTextures().prefilteredArray()},
             {19, _pointShadows.get()},
             {21, &_gbuffer->color(GBufferAttachment::TriangleId)}});
        _pbrResolveSet = descriptors.createPersistentTextureSet(
            {{1, &_gbuffer->color(GBufferAttachment::Diffuse)},
             {2, &_gbuffer->color(GBufferAttachment::EyeNormal)},
             {3, &_gbuffer->color(GBufferAttachment::Lightmap)},
             {4, &_gbuffer->color(GBufferAttachment::SelfIllum)},
             {5, &_gbuffer->depth()},
             {13, &_renderer.pbrTextures().brdfImage()},
             {15, _dirShadows.get()},
             {16, &_renderer.pbrTextures().irradianceArray()},
             {17, &_renderer.pbrTextures().prefilteredArray()},
             {19, _pointShadows.get()},
             {21, &_gbuffer->color(GBufferAttachment::TriangleId)},
             {TextureUnits::pointShadowRaw, _pointShadows.get(), {}, _pointShadowRawSampler}});
    }

    _outputHandle = std::make_shared<Texture>(
        _primaryRayMode ? "vk_primary_ray_output" : "vk_scene_output",
        TextureType::TwoDim, Texture::Properties());
    _renderer.resources().registerExternal(*_outputHandle, *_output);

    _renderer.immediateSubmit([this](ICommandBuffer &cmd) {
        cmd.transitionImage(*_output, ImageLayout::ShaderRead);
        cmd.transitionImage(*_tailColor, ImageLayout::ShaderRead);
    });

    // DLSS-RR occupies the same slot, but is the one occupant that can be
    // absent for reasons outside the build: the user has to supply Streamline's
    // DLLs and the adapter has to be an RTX part. Neither is an error, so an
    // unavailable RR quietly becomes FSR rather than emptying the slot.
    if (_options.antialiasing == AntiAliasing::DlssRr) {
        if (_renderer.rayReconstructionAvailable()) {
            try {
                _upscaler = _renderer.makeRayReconstructionUpscaler(_renderSize, _targetSize);
                _dlssRr = _upscaler != nullptr;
            } catch (const std::exception &e) {
                warn(std::string("DLSS Ray Reconstruction unavailable: ") + e.what(),
                     LogChannel::Graphics);
                _upscaler.reset();
                _dlssRr = false;
            }
        }
        if (!_upscaler) {
            info("Anti-aliasing is set to DLSS Ray Reconstruction, which is not available "
                 "here; falling back to FSR",
                 LogChannel::Graphics);
        }
    }

    if (_options.antialiasing == AntiAliasing::Fsr ||
        (_options.antialiasing == AntiAliasing::DlssRr && !_upscaler)) {
        try {
            // High dynamic range in every mode. FSR reads the scene image
            // before the display transform, and that image is now linear and
            // unbounded whichever pass produced it - which is precisely what
            // the flag declares. It was false for raster because raster used to
            // hand over display-referred colour already in [0,1]; that is no
            // longer true of any mode, and claiming otherwise would have FSR
            // reproject linear radiance with its HDR handling switched off.
            _upscaler = _renderer.makeUpscaler(_renderSize, _targetSize, true);
        } catch (const std::exception &e) {
            warn(std::string("Upscaler unavailable, the anti-aliasing slot is empty: ") + e.what(),
                 LogChannel::Graphics);
            _upscaler.reset();
        }
        if (!_upscaler) {
            // A build without the upscaler compiled in. Loud rather than
            // silent, because the frame that comes out has no anti-aliasing at
            // all and nothing else in the image says so.
            warn("Anti-aliasing wants a temporal upscaler, which this build does not "
                 "carry; the slot is empty",
                 LogChannel::Graphics);
        }
    }
    // No history on the first frame either; init leaves the flag clear and the
    // first dispatch is therefore a reset.
    _temporalHistoryValid = false;
    _prevCameraPosition = glm::vec3(0.0f);
    _ssaoKernel = buildSSAOKernel();
    _skyBinding = {};

    if (_options.mode != RenderMode::Retro) {
        // The shared storage-output channels, owned here so any provider can
        // fill them: the trace kernel writes them through its set 2, and the
        // PBR channels pass writes the same images through the resolve set.
        // Retro alone never shades into them and skips the allocation. Formats
        // mirror the trace kernel's set-2 declarations (tracing/outputs.slang);
        // normal/roughness rides RGBA16F, the FP form NRD's SNORM encoding
        // accepts. Double buffered so two frames in flight never write and
        // read the same texels.
        static constexpr Format kChannelFormats[kNumTracingChannels] {
            Format::R16G16B16A16Sfloat, // diffuse radiance + hit dist
            Format::R16G16B16A16Sfloat, // specular radiance + hit dist
            Format::R16G16B16A16Sfloat, // normal + roughness
            Format::R32Sfloat,          // viewZ
            Format::R16G16B16A16Sfloat, // motion
            Format::R16G16B16A16Sfloat, // noise-free
            Format::R16G16B16A16Sfloat, // diffuse material factor
            Format::R32Sfloat,          // device depth
            Format::R16G16B16A16Sfloat, // screen-space motion
            Format::R16G16B16A16Sfloat, // specular material factor
            Format::R8G8B8A8Unorm,      // canonical raster/tracer diffuse
            Format::R8G8B8A8Unorm,      // canonical packed eye normal
            Format::R32Sfloat,          // canonical positive linear view depth
            Format::R16G16Sfloat,       // canonical current-minus-previous UV motion
            Format::R16G16B16A16Sfloat, // direct diffuse + expected penumbra
        };
        for (int frame = 0; frame < 2; ++frame) {
            for (int i = 0; i < kNumTracingChannels; ++i) {
                auto image = _renderer.resources().makeImage();
                image->initColorAttachment(_renderSize, kChannelFormats[i]);
                _channelImages[frame][i] = std::move(image);
            }
        }
        // The composite, moved here from the tracer, and no longer behind the
        // NRD guard: the raster provider assembles through this same pass with
        // no denoiser in the picture, binding its raw channels where the
        // tracer binds NRD's outputs. Idiom A: a standalone compute pipeline
        // whose images change identity on resize, resolved by name. The order
        // matches the fill in compositePass.
        _compositePipeline = _renderer.makeComputePipeline({"composite", "main", 2});
        _compositeBindings = _compositePipeline->resolveBindings(
            {"outputImage", "inNoiseFree", "inDiffFactor", "inSpecFactor",
             "sDenoisedDiffuse", "sDenoisedSpecular", "inViewZ", "inRawDiffuse",
             "inRawSpecular", "sDirectDiffuse"});
    } else if (_dlssRr) {
        // Retro with RR in the slot: the one combination that needs guides and
        // has no provider writing them. RGBA16F for the normal because it holds
        // a signed unit vector and the roughness beside it, matching the
        // channel the other providers hand over; the specular albedo shares the
        // format for no better reason than that it is the same tag family and
        // is constant anyway.
        for (int frame = 0; frame < 2; ++frame) {
            for (int i = 0; i < 2; ++i) {
                auto image = _renderer.resources().makeImage();
                image->initColorAttachment(_renderSize, Format::R16G16B16A16Sfloat);
                _retroGuideImages[frame][i] = std::move(image);
            }
        }
        _retroGuidePipeline = _renderer.makeComputePipeline({"retro_guides", "main", 2});
        _retroGuideBindings = _retroGuidePipeline->resolveBindings(
            {"outNormalRoughness", "outSpecularAlbedo", "sGBufEyeNormal"});
    }

    _inited = true;
}
void ScenePipeline::deinit() {
    if (!_inited) {
        return;
    }
    // The descriptor owns a reference to the preview view, so release it
    // before that view. Engine teardown keeps ImGui alive until its pipelines
    // have done the same.
    if (_preview && _preview->imguiTexture) {
        _renderer.removePreviewTexture(_preview->imguiTexture);
    }
    _preview.reset();
    // Deregistered before the image goes: the registry holds a raw pointer to
    // it, keyed on the Texture, and would outlive both.
    if (_outputHandle) {
        _renderer.resources().unregisterExternal(*_outputHandle);
    }
    // Before the images it reprojects: the backend holds device objects of its
    // own and must not outlive the allocator either.
    _upscaler.reset();
    _output.reset();
    _tailColor.reset();
    _displayColor.reset();
    _displayTail.reset();
    _chainAtDisplaySize = false;
    _dirShadows.reset();
    _pointShadows.reset();
    for (auto &frame : _channelImages) {
        for (auto &image : frame)
            image.reset();
    }
    _compositePipeline.reset();
    _compositeBindings.clear();
    // Released beside the channels for the same reason and with the same
    // urgency. A rebuild that changes mode reaches init() again and takes a
    // different branch, so anything left here is not merely leaked - it is
    // read: upscalePass selects the retro guide provider on this pipeline being
    // non-null, and a Retro-to-PBR switch therefore tagged DLSS with images
    // sized for the previous pipeline. That is an access violation, not a
    // wrong picture.
    for (auto &frame : _retroGuideImages) {
        for (auto &image : frame)
            image.reset();
    }
    _retroGuidePipeline.reset();
    _retroGuideBindings.clear();
    _gbuffer.reset();
    _outputHandle.reset();
    // Persistent sets are not recycled by any per-frame pool reset, so a
    // pipeline that is thrown away on a graphics rebuild has to hand its own
    // back. Left leaked, a dozen rebuilds exhaust the pool and the next one
    // fails to allocate. The caller has waited the device idle before getting
    // here, so the GPU is done with them.
    if (_retroResolveSet)
        _renderer.descriptors().freePersistentTextureSet(_retroResolveSet);
    if (_pbrResolveSet)
        _renderer.descriptors().freePersistentTextureSet(_pbrResolveSet);
    _retroResolveSet = {};
    _pbrResolveSet = {};
    _resolveMaterialSet = {};
    _mergedScene = {};
    _mergedScenePrepared = false;
    _inited = false;
}

const GpuScene::View &ScenePipeline::prepareMergedScene(
    ICommandBuffer &cmd, ISceneCallbacks &callbacks) {
    if (_mergedScenePrepared) {
        return _mergedScene;
    }
    // Upload and compute-merge are recorded once before the first consumer.
    // GpuScene publishes the compute-to-vertex/index barrier; the same
    // buffers and descriptor set then feed shadows and the G-buffer.
    _mergedScene = callbacks.mergeGeometry(cmd);
    _mergedScenePrepared = true;
    _resolveMaterialSet = {};
    if (!_mergedScene.vertices.buffer || _mergedScene.triangleCount == 0) {
        return _mergedScene;
    }
    // The G-buffer names the triangle, not the material, and every consumer
    // reads the sentinel as "no geometry here". A merge that reached the
    // sentinel would paint its last triangle as sky.
    if (_mergedScene.triangleCount >= GBuffer::kNoTriangle) {
        warn("Vulkan: G-buffer R32_UINT triangle ID exhausted by " +
                 std::to_string(_mergedScene.triangleCount) +
                 " merged triangles; refusing to wrap into the 0xffffffff sentinel",
             LogChannel::Graphics);
        throw std::runtime_error(
            "Vulkan: too many triangles for the G-buffer triangle ID");
    }
    _resolveMaterialSet = _renderer.descriptors().updateMegaDrawSet(
        _renderer.frameIndex(), _mergedScene, _renderer.resources());
    return _mergedScene;
}

void ScenePipeline::shadowPass(ICommandBuffer &cmd,
                                     uint32_t globalsOffset,
                                     ISceneCallbacks &callbacks) {
    R_PROFILE_ZONE("ScenePipeline::shadowPass record");
    CommandBufferDebugScope debugScope(cmd, "shadowPass");
    if (_shadowCasters.empty()) {
        return;
    }
    const auto &scene = prepareMergedScene(cmd, callbacks);

    // One recording per caster. A view mask broadcasts a draw across the four
    // cascades or six faces a single caster owns, but it cannot span casters:
    // their maps sit at different layer bases, the two kinds are different
    // images, and the vertex stage picks its transforms from a push constant
    // that is fixed for the recording. Only occupied slots are rendered, which
    // is what keeps the cost proportional to the lights actually in range
    // rather than to the budget - see RECORD 3.13.
    bool dirBound = false;
    bool pointBound = false;
    for (const auto &caster : _shadowCasters) {
        // A caster whose map index is past what was allocated is a selection
        // bug, and the cheap consequence is the right one: rendering it would
        // address layers the image does not have, which resets the device with
        // no image and no identity. Dropping it costs one shadow and names
        // itself in the log - the same trade the bindless poison makes.
        const int allocated = caster.directional ? _dirShadowSlots : _pointShadowSlots;
        if (caster.mapIndex < 0 || caster.mapIndex >= allocated) {
            warn("Shadow caster " + std::to_string(caster.slot) + " wants " +
                     (caster.directional ? "directional" : "point") + " map " +
                     std::to_string(caster.mapIndex) + " of " + std::to_string(allocated) +
                     " allocated; skipped",
                 LogChannel::Graphics);
            continue;
        }
        auto &image = caster.directional ? *_dirShadows : *_pointShadows;
        bool &bound = caster.directional ? dirBound : pointBound;
        const int layers = caster.directional ? kNumShadowCascades : kNumCubeFaces;
        const uint32_t viewMask = (1u << layers) - 1u;
        const auto extent = image.extent();
        // Transitioned once per image, not once per caster: a second
        // transition of an image already in the attachment layout is a
        // redundant barrier, and the slots of one image are written by
        // consecutive passes that need no synchronisation between them.
        if (!bound) {
            cmd.transitionImage(image, ImageLayout::DepthAttachment);
            bound = true;
        }
        {
            const auto view = image.layerAttachmentView(caster.mapIndex * layers, layers);
            RenderAttachment depth {view, ImageLayout::DepthAttachment,
                                    AttachmentLoad::Clear, AttachmentStore::Store};
            depth.clear.depthOnly = true;
            cmd.beginRendering(extent, {}, &depth, viewMask, false);
            if (scene.vertices.buffer && scene.triangleCount != 0) {
            auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.frameIndex());
            std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
            offsets[UniformBlockBindingPoints::globals] = globalsOffset;
            cmd.bindIndexBuffer(*scene.indices.buffer, scene.indices.offset);

            const PipelineBinding meshPipeline = _renderer.pipelines().get(shadowPipelineKey(
                caster.directional ? "directionalShadowVertex" : "pointShadowVertex",
                caster.directional ? "directionalShadowFragment" : "pointShadowFragment",
                viewMask));

            auto drawRange = [&](uint32_t triangleBase, uint32_t triangleCount,
                                 bool gated) {
                if (triangleCount == 0) {
                    return;
                }
                const PipelineBinding &pipeline = meshPipeline;
                cmd.bindPipeline(pipeline.pipeline);
                cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                      offsets.data(), static_cast<uint32_t>(offsets.size()));
                cmd.bindDescriptorSet(pipeline.layout, 2, _resolveMaterialSet, nullptr, 0);
                // Three words here, two everywhere else: only this pass reads the
                // caster filter. See PushConstants in lib/megadraw_geometry.slang.
                const ShadowPushConstants push {triangleBase, gated ? 1u : 0u,
                                                _shadowCasterCategories,
                                                static_cast<uint32_t>(caster.slot)};
                cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
                cmd.drawIndexed(triangleCount * 3, triangleBase * 3);
            };

            drawRange(0, scene.opaqueTriangleCount, false);
            const uint32_t gatedTriangles =
                scene.triangleCount - scene.opaqueTriangleCount;
            drawRange(scene.opaqueTriangleCount, gatedTriangles, true);
            }
            // Grass casts into the directional cascades only. A point light's
            // cube would rasterize every card in the module, and its grass
            // shadow is sub-pixel at that map size.
            if (scene.grassCardCount != 0 && caster.directional) {
                const PipelineBinding pipeline = _renderer.pipelines().get(shadowPipelineKey(
                    "grassCardDirectionalShadowVertex", "directionalShadowFragment", viewMask));
                auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.frameIndex());
                std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
                offsets[UniformBlockBindingPoints::globals] = globalsOffset;
                cmd.bindPipeline(pipeline.pipeline);
                cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                      offsets.data(), static_cast<uint32_t>(offsets.size()));
                cmd.bindDescriptorSet(pipeline.layout, 2, _resolveMaterialSet, nullptr, 0);
                CommandBufferDebugScope grassScope(cmd, "grass cards");
                const ShadowPushConstants push {0, 1, _shadowCasterCategories,
                                                static_cast<uint32_t>(caster.slot)};
                cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
                cmd.draw(scene.grassCardTris * 3, scene.grassCardCount);
            }
            cmd.endRendering();
        }
    }
    if (dirBound) {
        cmd.transitionImage(*_dirShadows, ImageLayout::DepthRead);
    }
    if (pointBound) {
        cmd.transitionImage(*_pointShadows, ImageLayout::DepthRead);
    }
}

void ScenePipeline::geometryPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                                       ISceneCallbacks &callbacks) {
    R_PROFILE_ZONE("ScenePipeline::geometryPass record");
    CommandBufferDebugScope debugScope(cmd, "geometryPass");
    const auto &scene = prepareMergedScene(cmd, callbacks);

    transitionGBuffer(cmd, *_gbuffer, ImageLayout::ColorAttachment);
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthAttachment);
    std::vector<RenderAttachment> colors;
    colors.reserve(kGBufferAttachments.size());
    for (auto gbufferAttachment : kGBufferAttachments) {
        RenderAttachment attachment {
            _gbuffer->color(gbufferAttachment).sampleView(),
            ImageLayout::ColorAttachment, AttachmentLoad::Clear, AttachmentStore::Store};
        if (gbufferAttachment == GBufferAttachment::TriangleId) {
            attachment.clear.integer = true;
            attachment.clear.uintValue = GBuffer::kNoTriangle;
        }
        colors.push_back(attachment);
    }
    RenderAttachment depth {_gbuffer->depth().sampleView(), ImageLayout::DepthAttachment,
                            AttachmentLoad::Clear, AttachmentStore::Store};
    depth.clear.depthOnly = true;
    cmd.beginRendering(_renderSize, colors, &depth, 0, true);

    if (scene.vertices.buffer && scene.triangleCount != 0) {
        PipelineKey key;
        key.module = "scene_draw";
        key.vertexEntry = "sceneDrawVertex";
        key.fragmentEntry = "sceneDrawFragment";
        key.colorFormats = _gbuffer->colorFormats();
        key.depthFormat = _gbuffer->depthFormat();
        key.depthTest = true;
        key.depthWrite = true;
        key.cull = FaceCullMode::None;
        PipelineBinding pipeline = _renderer.pipelines().get(key);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.frameIndex());
        std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = globalsOffset;
        cmd.bindPipeline(pipeline.pipeline);
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, 2, _resolveMaterialSet, nullptr, 0);
        cmd.bindIndexBuffer(*scene.indices.buffer, scene.indices.offset);

        if (scene.opaqueTriangleCount != 0) {
            const MegaDrawPushConstants push {0, 0};
            cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
            cmd.drawIndexed(scene.opaqueTriangleCount * 3, 0);
        }
        const uint32_t nonOpaqueTriangles =
            scene.triangleCount - scene.opaqueTriangleCount;
        if (nonOpaqueTriangles != 0) {
            const MegaDrawPushConstants push {scene.opaqueTriangleCount, 1};
            cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
            cmd.drawIndexed(nonOpaqueTriangles * 3, scene.opaqueTriangleCount * 3);
        }
    }
    if (scene.grassCardCount != 0) {
        PipelineKey key;
        key.module = "scene_draw";
        key.vertexEntry = "grassCardDrawVertex";
        key.fragmentEntry = "sceneDrawFragment";
        key.colorFormats = _gbuffer->colorFormats();
        key.depthFormat = _gbuffer->depthFormat();
        key.depthTest = true;
        key.depthWrite = true;
        key.cull = FaceCullMode::None;
        PipelineBinding pipeline = _renderer.pipelines().get(key);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.frameIndex());
        std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = globalsOffset;
        cmd.bindPipeline(pipeline.pipeline);
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, 2, _resolveMaterialSet, nullptr, 0);
        CommandBufferDebugScope grassScope(cmd, "grass cards");
        const MegaDrawPushConstants push {0, 1};
        cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
        cmd.draw(scene.grassCardTris * 3, scene.grassCardCount);
    }
    cmd.endRendering();
    skyMotionPass(cmd, globalsOffset);
}

/**
 * The motion the sky owes the temporal resolve.
 *
 * Part of publishing the G-buffer rather than a step of its own, so that every
 * mode gets it from the one place primary visibility is rasterized - the raster
 * resolves and the tracer all read this same target, and none of them writes
 * it. Attached to the geometry pass instead of the plan for the same reason the
 * sky composite is not a step any more: the set of pixels involved is decided
 * by the triangle-id sentinel that pass just wrote, and nothing between here
 * and the temporal resolve can change it.
 *
 * Unconditional, rather than gated on the anti-aliasing dial that supplies its
 * only consumer today. The motion target is part of what the geometry pass
 * publishes, and --dumptargets and the debug motion channel both read it; a
 * target that is correct only when a dial happens to be set is the kind of
 * diagnostic that lies to whoever reaches for it next. The cost does not argue
 * otherwise - measured against nineteen extra copies of itself, because one is
 * far below the run-to-run spread, a single pass is around a tenth of a percent
 * of a raster frame.
 */
void ScenePipeline::skyMotionPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::skyMotionPass record");
    CommandBufferDebugScope debugScope(cmd, "skyMotionPass");
    auto &motion = _gbuffer->color(GBufferAttachment::Motion);
    auto &triangleId = _gbuffer->color(GBufferAttachment::TriangleId);
    cmd.transitionImage(triangleId, ImageLayout::ShaderRead);
    cmd.transitionImage(motion, ImageLayout::ColorAttachment);

    PipelineKey key;
    key.module = "sky";
    key.vertexEntry = "skyMotionVertex";
    key.fragmentEntry = "skyMotionFragment";
    key.colorFormats = {motion.pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    auto textureSet = _renderer.descriptors().acquireTextureDescriptorSet(
        _renderer.uniformRing().frame(), {{TextureUnits::gBufTriangleId, &triangleId}});

    {
        // Loaded, not cleared: the covered pixels hold the surface motion the
        // geometry pass wrote and the fragment discards over them.
        RenderAttachment color {motion.sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::Load, AttachmentStore::Store};
        cmd.beginRendering(_renderSize, {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(
            _renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, textureSet, nullptr, 0);
        cmd.draw(3, 1);
        cmd.endRendering();
    }
}

void ScenePipeline::blendedPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                                      ISceneCallbacks &callbacks) {
    R_PROFILE_ZONE("ScenePipeline::blendedPass record");
    CommandBufferDebugScope debugScope(cmd, "blendedPass");
    const auto &scene = prepareMergedScene(cmd, callbacks);
    const uint32_t nonOpaqueTriangles =
        scene.triangleCount > scene.opaqueTriangleCount
            ? scene.triangleCount - scene.opaqueTriangleCount
            : 0;
    if (scene.depthIndependentTriangleCount > nonOpaqueTriangles) {
        throw std::runtime_error(
            "Depth-independent triangle range exceeds non-opaque scene");
    }
    if (!scene.vertices.buffer || nonOpaqueTriangles == 0) {
        return;
    }
    // Depth-test against the opaque G-buffer but never write: blended
    // fragments must not reject each other, or the result depends on which
    // one happened to be drawn first rather than on coverage.
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
    cmd.transitionImage(*_output, ImageLayout::ColorAttachment);
    auto &motion = _gbuffer->color(GBufferAttachment::Motion);
    cmd.transitionImage(motion, ImageLayout::ColorAttachment);

    // The resolve already wrote this image; production loading preserves it.
    PipelineKey key;
    key.module = "scene_draw";
    key.vertexEntry = "sceneDrawVertex";
    key.fragmentEntry = "sceneDrawBlendedFragment";
    key.colorFormats = {_output->pixelFormat(), motion.pixelFormat()};
    key.depthFormat = _gbuffer->depthFormat();
    key.depthTest = true;
    key.depthWrite = false;
    key.blend = BlendMode::Premultiplied;
    key.cull = FaceCullMode::None;
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.frameIndex());
    const auto shadowSet = _options.mode == RenderMode::Retro
                               ? _retroResolveSet
                               : _pbrResolveSet;
    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;

    {
        RenderAttachment color {_output->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::Load, AttachmentStore::Store};
        RenderAttachment motionAttachment {
            motion.sampleView(), ImageLayout::ColorAttachment,
            AttachmentLoad::Load, AttachmentStore::Store};
        RenderAttachment depth {_gbuffer->depth().sampleView(), ImageLayout::DepthRead,
                                AttachmentLoad::Load, AttachmentStore::DontCare};
        cmd.beginRendering(_renderSize, {color, motionAttachment}, &depth, 0, true);
        cmd.bindIndexBuffer(*scene.indices.buffer, scene.indices.offset);
        const uint32_t depthTestedTriangles =
            nonOpaqueTriangles - scene.depthIndependentTriangleCount;
        if (depthTestedTriangles != 0) {
            cmd.bindPipeline(pipeline.pipeline);
            cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                  offsets.data(), static_cast<uint32_t>(offsets.size()));
            cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet,
                                  shadowSet, nullptr, 0);
            cmd.bindDescriptorSet(pipeline.layout, 2, _resolveMaterialSet, nullptr, 0);
            // Submission order, deliberately. See scene_draw.slang.
            const FogPushConstants fog = fogParameters();
            const BlendedPushConstants push {scene.opaqueTriangleCount, 2, resolveFlags(), 0,
                                             fog.density, fog.falloff, fog.planeZ};
            cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
            cmd.drawIndexed(depthTestedTriangles * 3,
                            scene.opaqueTriangleCount * 3);
        }
        debug("blended: opaque=" + std::to_string(scene.opaqueTriangleCount) +
                  " total=" + std::to_string(scene.triangleCount) +
                  " depthIndependent=" + std::to_string(scene.depthIndependentTriangleCount),
              LogChannel::Graphics);
        if (scene.depthIndependentTriangleCount != 0) {
            // Flare visibility was decided by the scene's walkmesh LOS query.
            // The old flare pass therefore disabled depth testing: testing the
            // endpoint against the richer opaque G-buffer rejects a halo at
            // the light or against render geometry the LOS query never saw.
            key.depthTest = false;
            PipelineBinding depthIndependentPipeline =
                _renderer.pipelines().get(key);
            cmd.bindPipeline(depthIndependentPipeline.pipeline);
            cmd.bindDescriptorSet(depthIndependentPipeline.layout,
                                  IDescriptors::kUniformSet, uniformSet,
                                  offsets.data(), static_cast<uint32_t>(offsets.size()));
            cmd.bindDescriptorSet(depthIndependentPipeline.layout,
                                  IDescriptors::kTextureSet, shadowSet, nullptr, 0);
            cmd.bindDescriptorSet(depthIndependentPipeline.layout, 2,
                                  _resolveMaterialSet, nullptr, 0);
            const uint32_t triangleBase =
                scene.triangleCount - scene.depthIndependentTriangleCount;
            const FogPushConstants fog = fogParameters();
            const BlendedPushConstants push {triangleBase, 2, resolveFlags(), 0,
                                             fog.density, fog.falloff, fog.planeZ};
            cmd.pushGraphicsConstants(depthIndependentPipeline.layout, &push,
                                      sizeof(push));
            cmd.drawIndexed(scene.depthIndependentTriangleCount * 3,
                            triangleBase * 3);
        }
        cmd.endRendering();
    }
    // Publish the composited image in the layout expected by the preview and
    // post-process descriptors, just as both resolve passes do after writing.
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
}

uint32_t ScenePipeline::resolveFlags() const {
    uint32_t flags = 0;
    if (_skyBinding.cube && _skyBinding.baked) {
        flags |= kResolveFlagSky;
    }
    if (_options.ssao) {
        flags |= kResolveFlagSSAO;
    }
    if (_transparentOutput) {
        flags |= kResolveFlagTransparentOutput;
    }
    if (_options.parityDirect) {
        flags |= kResolveFlagParityDirect;
    }
    if (_options.mode == RenderMode::Retro) {
        flags |= kResolveFlagDisplayReferred;
    }
    if (_options.fog && _fogEnabled) {
        flags |= kResolveFlagFog;
    }
    return flags;
}

FogPushConstants ScenePipeline::fogParameters() const {
    // Density from the area's authored far distance - a ground-level ray
    // reaches ~99% fog at that range, so a module keeps the reach it was
    // authored for - and falloff from the gradient height, the altitude at
    // which density has dropped to a hundredth. The plane is the walkmesh;
    // with no walkmesh the camera sits in it. One function, because the tail
    // pass and the blended pass must integrate the same fog.
    const auto &globals = _uniforms.globals();
    // Density calibrated against the ramp the area was authored for, at its
    // MIDPOINT: the original is linear from fogNear to fogFar and 50% fogged
    // halfway between them, so the exponential is set to agree there. It was
    // set to reach 99% at fogFar, which is the same curve scaled up roughly
    // threefold - measured as a tree at a fifth of the fog distance already
    // 60% fogged, where the authored ramp puts it near a tenth. The near and
    // far values shape the calibration only; the integral itself starts at
    // the camera, as an exponential must.
    const float midpoint = 0.5f * (std::max(0.0f, globals.fogNear) + std::max(1.0f, globals.fogFar));
    return {kFogOpticalDepthAtMidpoint / std::max(1.0f, midpoint),
            kFogOpticalDepthAtFar / std::max(0.05f, _options.fogHeight),
            _groundHeight.value_or(globals.cameraPosition.z)};
}

namespace {

/**
 * Every field, at every call site.
 *
 * The PBR resolve used to build this from resolveFlags() alone, which left the
 * remaining members value-initialised: thin surfaces transmitted zero light in
 * that mode while retro passed the authored fraction, so a blade of grass lit
 * from behind was black there and lit here for no reason either mode stated.
 */
/**
 * One run of text laid into the shared text block, exactly as Renderer2D lays
 * one out: a glyph rect in pixels and an atlas rect per character. @p origin
 * is relative to the label's anchor, which the vertex stage projects.
 */
int fillTextRun(Font &font, std::string_view text, glm::vec2 origin, TextUniforms &chars) {
    const int numChars = std::min(kMaxTextChars, static_cast<int>(text.size()));
    const auto &glyphs = font.glyphs();
    glm::vec2 offset = origin;
    for (int i = 0; i < numChars; ++i) {
        const auto &glyph = glyphs[static_cast<unsigned char>(text[i])];
        chars.chars[i].posScale =
            glm::vec4(offset.x, offset.y, glyph.size.x, glyph.size.y);
        chars.chars[i].uv = glm::vec4(glyph.ul.x, glyph.lr.y,
                                      glyph.lr.x - glyph.ul.x,
                                      glyph.ul.y - glyph.lr.y);
        offset.x += font.glyphAdvance(glyph, 1.0f);
    }
    return numChars;
}

ResolvePushConstants resolvePush(uint32_t flags, const GraphicsOptions &options) {
    return {flags,
            std::clamp(options.thinTransmission, 0.0f, 1.0f),
            std::clamp(options.albedoGamma, 0.1f, 4.0f),
            std::clamp(options.lightDistanceClamp, 0.0f, 1.0f),
            std::max(0.0f, options.pbrLightmapIntensity),
            // The tracer's dial, read by PBR too: the two modes are meant to
            // differ in how light reaches a surface, never in what the light
            // is, and a lamp of a different size in one of them is the latter.
            std::clamp(options.ptPointEmitterRatio, 0.01f, 0.5f),
            std::max(0.0f, options.pbrDirectIntensity),
            std::max(0.0f, options.pbrSunIntensity),
            std::max(0.0f, options.skyboxIntensity),
            std::clamp(options.skyboxGamma, 0.1f, 4.0f),
            0.0f};
}

} // namespace

GBufferBinding ScenePipeline::gbufferBinding() {
    GBufferBinding binding;
    binding.diffuse = &_gbuffer->color(GBufferAttachment::Diffuse);
    binding.eyeNormal = &_gbuffer->color(GBufferAttachment::EyeNormal);
    binding.lightmap = &_gbuffer->color(GBufferAttachment::Lightmap);
    binding.selfIllum = &_gbuffer->color(GBufferAttachment::SelfIllum);
    binding.motion = &_gbuffer->color(GBufferAttachment::Motion);
    binding.depth = &_gbuffer->depth();
    binding.triangleId = &_gbuffer->color(GBufferAttachment::TriangleId);
    return binding;
}

DescriptorSet ScenePipeline::resolveSet(IImage *output) {
    return _renderer.descriptors().acquireResolveDescriptorSet(
        _renderer.uniformRing().frame(), output, _skyBinding.cube, _skyBinding.view);
}

void ScenePipeline::retroResolvePass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::retroResolvePass record");
    CommandBufferDebugScope debugScope(cmd, "retroResolvePass");
    transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
    cmd.transitionImage(*_output, ImageLayout::ColorAttachment);

    // Deliberately still a fragment pass. Retro is the original's lighting
    // model and this project's fixed reference for it; it has no occlusion term
    // to fold in and writes one value per pixel from one read of the G-buffer,
    // so a dispatch would buy it nothing and cost it a rewrite. The asymmetry
    // with the PBR resolve beside it is the point, not an oversight.
    PipelineKey key;
    key.module = "retro_resolve";
    key.vertexEntry = "retroResolveVertex";
    key.fragmentEntry = "retroResolveFragment";
    key.colorFormats = {_output->pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    // No storage image: this pass writes an attachment, and the binding is
    // partially bound precisely so it can be left out here.
    auto skySet = resolveSet(nullptr);

    {
        RenderAttachment color {_output->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::Clear, AttachmentStore::Store};
        color.clear.color = {0.0f, 0.0f, 0.0f, 1.0f};
        cmd.beginRendering(_renderSize, {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, _retroResolveSet, nullptr, 0);
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kResolveSet, skySet, nullptr, 0);
        if (_resolveMaterialSet) {
            cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kMegaDrawSet,
                                  _resolveMaterialSet, nullptr, 0);
            const ResolvePushConstants push = resolvePush(resolveFlags(), _options);
            cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
            cmd.draw(3, 1);
        }
        cmd.endRendering();
    }
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
    retroGuidePass(cmd);
}

void ScenePipeline::retroGuidePass(ICommandBuffer &cmd) {
    if (!_retroGuidePipeline) {
        return;
    }
    R_PROFILE_ZONE("ScenePipeline::retroGuidePass record");
    CommandBufferDebugScope debugScope(cmd, "retroGuidePass");
    const int frame = _renderer.frameIndex();
    auto &normalRoughness = *_retroGuideImages[frame][0];
    auto &specularAlbedo = *_retroGuideImages[frame][1];
    cmd.transitionImage(normalRoughness, ImageLayout::General);
    cmd.transitionImage(specularAlbedo, ImageLayout::General);
    // The G-buffer is already ShaderRead here - the resolve above put it there
    // and this pass reads the same attachment it did.
    auto &eyeNormal = _gbuffer->color(GBufferAttachment::EyeNormal);

    const std::array<ComputeBinding, 3> bindings {{
        {_retroGuideBindings[0], normalRoughness.sampleView()},
        {_retroGuideBindings[1], specularAlbedo.sampleView()},
        {_retroGuideBindings[2], eyeNormal.sampleView()},
    }};
    struct RetroGuidePushConstants {
        glm::mat4 viewInv;
        // See the shader: the matrix aligns what follows to 16 bytes, so this
        // is a uvec4 on both sides rather than a pair of uints on one and an
        // 80-byte reflected block on the other.
        glm::uvec4 extent;
    } push {_uniforms.globals().viewInv,
            glm::uvec4(static_cast<uint32_t>(_renderSize.x),
                       static_cast<uint32_t>(_renderSize.y), 0u, 0u)};
    static_assert(sizeof(RetroGuidePushConstants) == 80,
                  "push constant block must stay free of padding");
    cmd.dispatch(*_retroGuidePipeline,
                 {(_renderSize.x + kResolveGroupSize - 1) / kResolveGroupSize,
                  (_renderSize.y + kResolveGroupSize - 1) / kResolveGroupSize, 1},
                 {bindings.data(), static_cast<uint32_t>(bindings.size())},
                 nullptr, &push, sizeof(push));
    cmd.transitionImage(normalRoughness, ImageLayout::ShaderRead);
    cmd.transitionImage(specularAlbedo, ImageLayout::ShaderRead);
}

void ScenePipeline::pbrChannelsPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::pbrChannelsPass record");
    CommandBufferDebugScope debugScope(cmd, "pbrChannelsPass");
    _tracingOutput = {};
    if (!_resolveMaterialSet) {
        // No merged geometry: nothing to shade. Publish the black the resolve
        // publishes and tell the composite to stand aside.
        cmd.transitionImage(*_output, ImageLayout::General);
        cmd.clearColor(*_output, {0.0f, 0.0f, 0.0f, _transparentOutput ? 0.0f : 1.0f});
        cmd.transitionImage(*_output, ImageLayout::ShaderRead);
        return;
    }
    transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);

    // The channel images this dispatch writes, in the resolve-set order the
    // shader declares (bindings 2..9): noiseFree, diffuse, specular,
    // directDiffuse, diffFactor, specFactor, viewZ, normalRoughness - mapped
    // onto the shared channel-image indices the composite reads.
    //
    // The last is written for DLSS-RR alone; the composite never reads it. It
    // is the same image the tracer fills, because NRD's encoding takes a plain
    // world normal and linear roughness, so one guide serves both providers.
    const auto channels = acquireChannelBinding();
    const std::array<IImage *, 8> resolveChannels {{
        channels[ChannelSlot::NoiseFree],
        channels[ChannelSlot::Diffuse],
        channels[ChannelSlot::Specular],
        channels[ChannelSlot::DirectDiffuse],
        channels[ChannelSlot::DiffFactor],
        channels[ChannelSlot::SpecFactor],
        channels[ChannelSlot::ViewZ],
        channels[ChannelSlot::NormalRoughness],
    }};
    cmd.transitionImages({resolveChannels.begin(), resolveChannels.end()},
                         ImageLayout::General);

    PipelineKey key;
    key.module = "pbr_channels";
    key.computeEntry = "channelsMain";
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    // The occlusion kernel, exactly as the single-image resolve pushes it.
    ScreenEffectUniforms screenEffect;
    screenEffect.screenResolution = glm::vec2(_renderSize);
    screenEffect.screenResolutionRcp = 1.0f / glm::vec2(_renderSize);
    screenEffect.clipNear = _uniforms.globals().clipNear;
    screenEffect.clipFar = _uniforms.globals().clipFar;
    std::copy(_ssaoKernel.begin(), _ssaoKernel.end(), screenEffect.ssaoSamples);
    auto screenEffectOffset = _renderer.uniformRing().push(screenEffect);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    offsets[UniformBlockBindingPoints::screenEffect] = screenEffectOffset;

    auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
    auto resolveChannelSet = _renderer.descriptors().acquireResolveDescriptorSet(
        _renderer.uniformRing().frame(), nullptr, _skyBinding.cube, _skyBinding.view,
        resolveChannels.data(), static_cast<uint32_t>(resolveChannels.size()));
    cmd.bindComputePipeline(pipeline.pipeline);
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                 offsets.data(), static_cast<uint32_t>(offsets.size()));
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, _pbrResolveSet,
                                 nullptr, 0);
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kMegaDrawSet,
                                 _resolveMaterialSet, nullptr, 0);
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kResolveSet,
                                 resolveChannelSet, nullptr, 0);
    const ResolvePushConstants push = resolvePush(resolveFlags(), _options);
    cmd.pushComputeConstants(pipeline.layout, &push, sizeof(push));
    cmd.dispatchCompute({(_renderSize.x + kResolveGroupSize - 1) / kResolveGroupSize,
                         (_renderSize.y + kResolveGroupSize - 1) / kResolveGroupSize, 1});

    // The writes above feed the composite's reads.
    for (auto *image : resolveChannels) {
        cmd.imageBarrier(*image, ImageUse::ComputeStore, ImageUse::ComputeRead);
    }

    // No denoiser in this mode: the composite reads the raw channels where the
    // tracer hands it NRD's outputs, at zero jitter, which makes its sampled
    // reads the raw texel at the pixel centre - the contract line exactly.
    _tracingOutput.runComposite = true;
    _tracingOutput.denoisedDiffuse = channels[ChannelSlot::Diffuse]->sampleView();
    _tracingOutput.denoisedSpecular = channels[ChannelSlot::Specular]->sampleView();
    _tracingOutput.directDiffuse = channels[ChannelSlot::DirectDiffuse]->sampleView();
}

ChannelBinding ScenePipeline::acquireChannelBinding() {
    const int frame = _renderer.frameIndex();
    ChannelBinding binding;
    for (int i = 0; i < kNumTracingChannels; ++i) {
        binding.images[i] = _channelImages[frame][i].get();
    }
    _frameChannels = binding;
    _lastChannelFrame = frame;
    return binding;
}

void ScenePipeline::compositePass(ICommandBuffer &cmd) {
    CommandBufferDebugScope debugScope(cmd, "compositePass");
    if (!_compositePipeline) {
        return;
    }
    // The channel images this pipeline owns, plus the denoised pair and direct
    // view the tracer handed back. The binding order is the one resolved in
    // init(); the tracer used to bind exactly this set from its own copies.
    const auto &channels = _frameChannels;

    // DLSS-RR denoises as well as resolves, so where it runs it wants the
    // NOISY assembly - the same raw channels the PBR provider hands over. Take
    // them back from whatever the tracer settled on, and drop the jitter with
    // them: at zero offset the composite's sampled reads land on the pixel
    // centre, which is the raw texel.
    //
    // NRD is left running rather than skipped. It can only be here at all in a
    // developer build that also enabled it, its output is simply unread on
    // this path, and the alternative - suppressing it from here - would make
    // an RR that failed to start fall back to FSR with no denoiser behind it,
    // which is a far worse failure than some wasted milliseconds.
    if (_dlssRr && _lastChannelFrame >= 0) {
        _tracingOutput.denoisedDiffuse = channels[ChannelSlot::Diffuse]->sampleView();
        _tracingOutput.denoisedSpecular = channels[ChannelSlot::Specular]->sampleView();
        _tracingOutput.directDiffuse = channels[ChannelSlot::DirectDiffuse]->sampleView();
        _tracingOutput.denoisedJitter[0] = 0.0f;
        _tracingOutput.denoisedJitter[1] = 0.0f;
        _tracingOutput.directDenoised = 0;
    }
    const std::array<ComputeBinding, 10> compositeBindings {{
        {_compositeBindings[0], _output->sampleView()},
        {_compositeBindings[1], channels[ChannelSlot::NoiseFree]->sampleView()},
        {_compositeBindings[2], channels[ChannelSlot::DiffFactor]->sampleView()},
        {_compositeBindings[3], channels[ChannelSlot::SpecFactor]->sampleView()},
        {_compositeBindings[4], _tracingOutput.denoisedDiffuse},
        {_compositeBindings[5], _tracingOutput.denoisedSpecular},
        {_compositeBindings[6], channels[ChannelSlot::ViewZ]->sampleView()},
        {_compositeBindings[7], channels[ChannelSlot::Diffuse]->sampleView()},
        {_compositeBindings[8], channels[ChannelSlot::Specular]->sampleView()},
        {_compositeBindings[9], _tracingOutput.directDiffuse},
    }};
    // Mirrors CompositePushConstants in slang/composite.slang: the denoiser
    // values the trace pass computed, and nothing else. Fog left this block
    // when it became a tail pass.
    struct CompositePushConstants {
        float denoisedJitter[2];
        uint32_t debugView;
        uint32_t directDenoised;
    } resolveConstants {
        {_tracingOutput.denoisedJitter[0], _tracingOutput.denoisedJitter[1]},
        _tracingOutput.debugView,
        _tracingOutput.directDenoised};
    static_assert(sizeof(CompositePushConstants) == 16,
                  "push constant block must stay free of padding");
    cmd.dispatch(*_compositePipeline,
                 {(_renderSize.x + kResolveGroupSize - 1) / kResolveGroupSize,
                  (_renderSize.y + kResolveGroupSize - 1) / kResolveGroupSize, 1},
                 {compositeBindings.data(), static_cast<uint32_t>(compositeBindings.size())},
                 nullptr, &resolveConstants, sizeof(resolveConstants));
}

void ScenePipeline::primaryCoveragePass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    CommandBufferDebugScope debugScope(cmd, "primaryCoveragePass");
    if (!_transparentOutput) {
        return;
    }
    // The tracer writes alpha one for every pixel, including a miss. Correct
    // it before the forward pass: normal transparent draws then accumulate
    // coverage over this base, while additive draws retain their zero alpha.
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
    auto &triangleId = _gbuffer->color(GBufferAttachment::TriangleId);
    cmd.transitionImage(triangleId, ImageLayout::ShaderRead);
    cmd.transitionImage(*_tailColor, ImageLayout::ColorAttachment);

    PipelineKey key;
    key.module = kPostProcessModule;
    key.vertexEntry = "postVertex";
    key.fragmentEntry = "primaryCoverageFragment";
    key.colorFormats = {_tailColor->pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    auto sourceSet = _renderer.descriptors().acquireTextureDescriptorSet(
        _renderer.uniformRing().frame(),
        {{TextureUnits::mainTex, _output.get()},
         {TextureUnits::gBufTriangleId, &triangleId}});
    const CoveragePushConstants push {_skyBinding.baked ? 1u : 0u};
    {
        RenderAttachment color {_tailColor->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::DontCare, AttachmentStore::Store};
        cmd.beginRendering(chainSize(), {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet, nullptr, 0);
        cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
        cmd.draw(3, 1);
        cmd.endRendering();
    }
    cmd.transitionImage(*_tailColor, ImageLayout::ShaderRead);
    std::swap(_output, _tailColor);
}

void ScenePipeline::coveragePass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    CommandBufferDebugScope debugScope(cmd, "coveragePass");
    if (!_transparentOutput) {
        return;
    }
    // FSR2 writes alpha one, so the display-sized colour needs coverage from
    // the render-sized source it consumed. _displayColor holds that source
    // after upscalePass swaps the display pair into the tail; without FSR the
    // completed output has carried alpha through every tail pass itself.
    IImage *coverage = _chainAtDisplaySize ? _displayColor.get() : _output.get();
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
    cmd.transitionImage(*coverage, ImageLayout::ShaderRead);
    cmd.transitionImage(*_tailColor, ImageLayout::ColorAttachment);

    PipelineKey key;
    key.module = kPostProcessModule;
    key.vertexEntry = "postVertex";
    key.fragmentEntry = "coverageFragment";
    key.colorFormats = {_tailColor->pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    auto sourceSet = _renderer.descriptors().acquireTextureDescriptorSet(
        _renderer.uniformRing().frame(),
        {{TextureUnits::mainTex, _output.get()},
         {TextureUnits::coverage, coverage}});
    {
        RenderAttachment color {_tailColor->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::DontCare, AttachmentStore::Store};
        cmd.beginRendering(chainSize(), {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet, nullptr, 0);
        cmd.draw(3, 1);
        cmd.endRendering();
    }
    cmd.transitionImage(*_tailColor, ImageLayout::ShaderRead);
    std::swap(_output, _tailColor);
}

void ScenePipeline::screenSpaceReflectionPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::screenSpaceReflectionPass record");
    CommandBufferDebugScope debugScope(cmd, "screenSpaceReflectionPass");
    // Reads the resolved image, writes the tail target, and the two exchange
    // identities - the same ping-pong every tail pass uses, for the same reason:
    // a kernel cannot march over an image it is writing.
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
    cmd.transitionImage(*_tailColor, ImageLayout::General);
    // Already published by the resolve on any frame that had geometry to shade,
    // and a no-op when they are; stated here because this pass reads them and a
    // degenerate frame reaches it without a resolve having run.
    transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);

    PipelineKey key;
    key.module = "ssr";
    key.computeEntry = "ssrMain";
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    ScreenEffectUniforms screenEffect;
    screenEffect.screenResolution = glm::vec2(_renderSize);
    screenEffect.screenResolutionRcp = 1.0f / glm::vec2(_renderSize);
    screenEffect.clipNear = _uniforms.globals().clipNear;
    screenEffect.clipFar = _uniforms.globals().clipFar;
    auto screenEffectOffset = _renderer.uniformRing().push(screenEffect);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    offsets[UniformBlockBindingPoints::screenEffect] = screenEffectOffset;

    // The march needs the lit colour, plus the depth and normal of the pixel it
    // starts from and the diffuse alpha that says how much of a mirror it is.
    // Units are the resolve's own numbering, which the module declares once.
    auto sourceSet = _renderer.descriptors().acquireTextureDescriptorSet(
        _renderer.uniformRing().frame(),
        {{TextureUnits::mainTex, _output.get()},
         {1, &_gbuffer->color(GBufferAttachment::Diffuse)},
         {2, &_gbuffer->color(GBufferAttachment::EyeNormal)},
         {5, &_gbuffer->depth()}});

    auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
    cmd.bindComputePipeline(pipeline.pipeline);
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                 offsets.data(), static_cast<uint32_t>(offsets.size()));
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet,
                                 nullptr, 0);
    // Unused by this kernel - it shades nothing - but bound while it is there,
    // so the set the layout declares is never left dangling.
    if (_resolveMaterialSet) {
        cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kMegaDrawSet,
                                     _resolveMaterialSet, nullptr, 0);
    }
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kResolveSet,
                                 resolveSet(_tailColor.get()), nullptr, 0);
    const ResolvePushConstants push = resolvePush(resolveFlags(), _options);
    cmd.pushComputeConstants(pipeline.layout, &push, sizeof(push));
    cmd.dispatchCompute({(_renderSize.x + kResolveGroupSize - 1) / kResolveGroupSize,
                         (_renderSize.y + kResolveGroupSize - 1) / kResolveGroupSize, 1});

    cmd.transitionImage(*_tailColor, ImageLayout::ShaderRead);
    std::swap(_output, _tailColor);
}

namespace {

/** Mirrors BloomPushConstants in postprocess.slang. */
struct BloomPushConstants {
    float threshold;
    float intensity;
    float texelStepX;
    float texelStepY;
};
static_assert(sizeof(BloomPushConstants) <= kCachedPipelinePushConstantSize);

} // namespace

void ScenePipeline::bloomPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::bloomPass record");
    CommandBufferDebugScope debugScope(cmd, "bloomPass");
    const glm::ivec2 size = chainSize();
    const glm::vec2 texel = 1.0f / glm::vec2(glm::max(size, glm::ivec2(1)));

    // One helper for all four steps: they differ only in what they read, what
    // they write, and which entry point runs.
    auto run = [&](const char *entry, IImage &target,
                   const std::vector<TextureBinding> &sources,
                   const BloomPushConstants &push) {
        for (const auto &source : sources) {
            cmd.transitionImage(const_cast<IImage &>(*source.image), ImageLayout::ShaderRead);
        }
        cmd.transitionImage(target, ImageLayout::ColorAttachment);

        PipelineKey key;
        key.module = kPostProcessModule;
        key.vertexEntry = "postVertex";
        key.fragmentEntry = entry;
        key.colorFormats = {target.pixelFormat()};
        PipelineBinding pipeline = _renderer.pipelines().get(key);

        std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
        offsets[UniformBlockBindingPoints::globals] = globalsOffset;
        auto textureSet = _renderer.descriptors().acquireTextureDescriptorSet(
            _renderer.uniformRing().frame(), sources);

        RenderAttachment color {target.sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::DontCare, AttachmentStore::Store};
        cmd.beginRendering(size, {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, textureSet, nullptr, 0);
        cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
        cmd.draw(3, 1);
        cmd.endRendering();
        cmd.transitionImage(target, ImageLayout::ShaderRead);
    };

    const float threshold = std::max(0.0f, _options.bloomThreshold);
    const float intensity = std::max(0.0f, _options.bloomIntensity);

    // Extract, gated on the self-illum channel the G-buffer already carries.
    run("bloomExtractFragment", *_bloomA,
        {{0, _output.get()}, {4, &_gbuffer->color(GBufferAttachment::SelfIllum)}},
        {threshold, intensity, 0.0f, 0.0f});
    // Separable blur: horizontal, then vertical back into the first image.
    run("bloomBlurFragment", *_bloomB, {{0, _bloomA.get()}},
        {threshold, intensity, texel.x, 0.0f});
    run("bloomBlurFragment", *_bloomA, {{0, _bloomB.get()}},
        {threshold, intensity, 0.0f, texel.y});
    // Composite over the scene, onto the tail target, and swap as the other
    // tail passes do.
    run("bloomCompositeFragment", *_tailColor,
        {{0, _output.get()}, {TextureUnits::hilights, _bloomA.get()}},
        {threshold, intensity, 0.0f, 0.0f});
    std::swap(_output, _tailColor);
}

void ScenePipeline::tailPass(ICommandBuffer &cmd, const char *fragmentEntry,
                             uint32_t globalsOffset, uint32_t screenEffectOffset,
                             const void *pushConstants, uint32_t pushConstantSize,
                             bool bindDepth) {
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
    if (bindDepth) {
        cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
    }
    cmd.transitionImage(*_tailColor, ImageLayout::ColorAttachment);

    PipelineKey key;
    key.module = kPostProcessModule;
    key.vertexEntry = "postVertex";
    key.fragmentEntry = fragmentEntry;
    key.colorFormats = {_tailColor->pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    offsets[UniformBlockBindingPoints::screenEffect] = screenEffectOffset;
    auto sourceSet =
        bindDepth ? _renderer.descriptors().acquireTextureDescriptorSet(
                        _renderer.uniformRing().frame(),
                        {{TextureUnits::mainTex, _output.get()},
                         {TextureUnits::gBufDepth, &_gbuffer->depth()}})
                  : _renderer.descriptors().acquireTextureDescriptorSet(
                        _renderer.uniformRing().frame(), _output.get());

    {
        // Every pixel is written, so the previous contents of the target are
        // never read back.
        RenderAttachment color {_tailColor->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::DontCare, AttachmentStore::Store};
        cmd.beginRendering(chainSize(), {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet, nullptr, 0);
        if (pushConstants && pushConstantSize != 0) {
            cmd.pushGraphicsConstants(pipeline.layout, pushConstants, pushConstantSize);
        }
        cmd.draw(3, 1);
        cmd.endRendering();
    }
    cmd.transitionImage(*_tailColor, ImageLayout::ShaderRead);
    // The pair are interchangeable, so the result becomes the output by
    // exchanging the handles rather than by copying pixels back.
    std::swap(_output, _tailColor);
}

void ScenePipeline::restartTemporalHistory() {
    _temporalHistoryValid = false;
}

void ScenePipeline::upscalePass(ICommandBuffer &cmd) {
    R_PROFILE_ZONE("ScenePipeline::upscalePass record");
    CommandBufferDebugScope debugScope(cmd, "upscalePass");
    if (!_upscaler) {
        return;
    }
    const auto &globals = _uniforms.globals();
    auto &motion = _gbuffer->color(GBufferAttachment::Motion);
    auto &depth = _gbuffer->depth();

    // Primary visibility is rasterized in every mode, traced included, so
    // these two attachments describe the same surfaces the colour was shaded
    // for whichever pass shaded it. That invariant is what lets one temporal
    // resolve serve every mode.
    //
    // The backend transitions each image from the state it is told it is in,
    // so all three inputs are published as plain sampled images first: the
    // depth attachment otherwise sits in a depth-read layout that no upscaler
    // state maps to. They are left sampled afterwards and the depth is put
    // back before anything reads it as a depth attachment again.
    // Upscaling writes into the display-resolution pair rather than the tail
    // it reads from; at NativeAA the two resolutions are equal and there is no
    // second pair, so it stays the ping-pong it always was.
    const bool upscaling = _renderSize != _targetSize;
    IImage &target = upscaling ? *_displayColor : *_tailColor;

    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
    cmd.transitionImage(motion, ImageLayout::ShaderRead);
    cmd.transitionImage(depth, ImageLayout::ShaderRead);
    cmd.transitionImage(target, ImageLayout::General);

    UpscalerInputs inputs;
    inputs.color = _output.get();
    inputs.depth = &depth;
    inputs.motion = &motion;
    inputs.output = &target;
    // The G-buffer stores current minus previous, as half a clip-space delta
    // with y up. FSR wants previous minus current, in pixels, with y down.
    // Both corrections are a sign per axis, so they ride in the scale rather
    // than in a rewrite of an attachment several passes and both dump paths
    // already read.
    inputs.motionScale = {-static_cast<float>(_renderSize.x),
                          static_cast<float>(_renderSize.y)};

    // The guides only DLSS-RR reads. It is denoising here as well as resolving,
    // so it needs the material factors the colour was modulated by and the
    // surface it was shaded on.
    //
    // Two providers, because there are two shapes of frame. Where a channel
    // provider ran - the tracer, or PBR's channels pass - the guides are the
    // channels it wrote. Retro fills no channels, and used to reach RR with no
    // guides at all: slEvaluateFeature then returned eErrorMissingInputParameter
    // on every frame and the resolve never ran, so the mode was offered and did
    // not work. retroGuidePass supplies the two it cannot otherwise produce,
    // and the G-buffer's diffuse attachment is tagged directly as the third -
    // it already is the albedo retro's resolve multiplies its lighting by.
    if (_dlssRr && _retroGuidePipeline) {
        const int chFrame = _renderer.frameIndex();
        inputs.diffuseAlbedo = &_gbuffer->color(GBufferAttachment::Diffuse);
        inputs.specularAlbedo = _retroGuideImages[chFrame][1].get();
        inputs.normalRoughness = _retroGuideImages[chFrame][0].get();
    } else if (_dlssRr && _lastChannelFrame >= 0) {
        const auto &channels = _frameChannels;
        inputs.diffuseAlbedo = channels[ChannelSlot::DiffFactor];
        inputs.specularAlbedo = channels[ChannelSlot::SpecFactor];
        inputs.normalRoughness = channels[ChannelSlot::NormalRoughness];
    }
    if (_dlssRr) {
        if (inputs.diffuseAlbedo) {
            cmd.transitionImage(*inputs.diffuseAlbedo, ImageLayout::ShaderRead);
        }
        if (inputs.specularAlbedo) {
            cmd.transitionImage(*inputs.specularAlbedo, ImageLayout::ShaderRead);
        }
        if (inputs.normalRoughness) {
            cmd.transitionImage(*inputs.normalRoughness, ImageLayout::ShaderRead);
        }
        // Unjittered at both ends: SL takes the sub-pixel offset separately, so
        // a jittered matrix would count it twice. globals.projection carries
        // the jitter as a clip translate, which is undone the same way the
        // tracer undoes it for NRD.
        const glm::mat4 unjitteredProjection =
            glm::translate(glm::vec3(-globals.jitter.x, -globals.jitter.y, 0.0f)) *
            globals.projection;
        inputs.view = globals.view;
        inputs.projection = unjitteredProjection;
        inputs.prevView = _prevView;
        inputs.prevProjection = _prevProjection;
        inputs.cameraPosition = glm::vec3(globals.cameraPosition);
        _prevView = globals.view;
        _prevProjection = unjitteredProjection;
    }

    // The sub-pixel offset this frame's projection was built with, in pixels
    // with y down. Non-zero exactly when this pass is FSR, which is the rule
    // SceneGraph::computeJitter applies - so the offset the projection carried
    // is the offset handed over here, with no dial in between to disagree.
    const glm::vec2 jitterPixels {globals.jitter.x * 0.5f * static_cast<float>(_renderSize.x),
                                  -globals.jitter.y * 0.5f * static_cast<float>(_renderSize.y)};
    const float verticalFov =
        2.0f * std::atan(1.0f / std::max(1e-4f, globals.projection[1][1]));

    // History is worthless across a cut, and a teleport-sized step is a cut
    // whether or not anything announced one.
    // As in the denoiser: the scene being emptied restarts this explicitly, so
    // what this catches is a cut that keeps the same scene.
    const auto cameraPosition = glm::vec3(globals.cameraPosition);
    if (glm::distance(cameraPosition, _prevCameraPosition) > 20.0f) {
        _temporalHistoryValid = false;
    }
    _prevCameraPosition = cameraPosition;
    const bool reset = !_temporalHistoryValid;
    _temporalHistoryValid = true;

    _upscaler->dispatch(cmd, inputs, jitterPixels, 1.0f / 60.0f,
                        globals.clipNear, globals.clipFar, verticalFov,
                        std::clamp(_options.sharpness, 0.0f, 1.0f), reset);

    cmd.transitionImage(target, ImageLayout::ShaderRead);
    cmd.transitionImage(depth, ImageLayout::DepthRead);
    if (upscaling) {
        // Hand the display pair to the tail. Everything after this pass reads
        // _output and writes _tailColor exactly as before and never learns
        // that the resolution changed under it; chainSize() is what tells the
        // render passes how big they now are. render() puts it back.
        std::swap(_output, _displayColor);
        std::swap(_tailColor, _displayTail);
        _chainAtDisplaySize = true;
    } else {
        std::swap(_output, _tailColor);
    }
}

void ScenePipeline::antiAliasingPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::antiAliasingPass record");
    CommandBufferDebugScope debugScope(cmd, "antiAliasingPass");
    // A new resolve in this slot is a new case here; the ping-pong is common.
    // Nothing in this slot applies a display transform, in any mode - that
    // belongs to the post-process pass alone, and a resolve that tonemapped to
    // find edges would be applying it twice.
    //
    // Where the slot SITS depends on which resolve occupies it, and that is
    // physics rather than a wart. A temporal resolve reprojects and accumulates
    // radiance, so it wants pre-tonemap linear colour and the widest range it
    // can get: FSR runs before the display transform. A spatial filter judges
    // the picture by luma contrast against fixed thresholds, so it wants the
    // encoded image a viewer sees: FXAA runs after it. On linear input FXAA's
    // edge detection under-triggers exactly where the encode compresses most,
    // which is the bright edges it is most needed on. RenderPipeline::render
    // places the step accordingly.
    const char *fragmentEntry = nullptr;
    switch (_options.antialiasing) {
    case AntiAliasing::None:
        return;
    case AntiAliasing::Fsr:
    case AntiAliasing::DlssRr:
        // One pass for both: the slot holds whichever resolver init() built,
        // and DLSS-RR falling back to FSR must reach exactly the same code
        // here. Leaving this case out cost a silent no-upscale path that was
        // only visible as a 12.2 mean difference against an 0.014 noise floor.
        upscalePass(cmd);
        return;
    case AntiAliasing::Fxaa:
        fragmentEntry = "fxaaFragment";
        break;
    }
    if (!fragmentEntry) {
        return;
    }
    // FXAA works off neighbouring texel offsets, which nothing else in this
    // pipeline publishes; the block is otherwise irrelevant to the pass.
    ScreenEffectUniforms screenEffect;
    screenEffect.screenResolution = glm::vec2(chainSize());
    screenEffect.screenResolutionRcp = 1.0f / glm::vec2(chainSize());
    auto screenEffectOffset = _renderer.uniformRing().push(screenEffect);
    tailPass(cmd, fragmentEntry, globalsOffset, screenEffectOffset, nullptr, 0);
}

void ScenePipeline::postProcessPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::postProcessPass record");
    CommandBufferDebugScope debugScope(cmd, "postProcessPass");
    // Unconditional, and the same in every mode: the scene chain stops at
    // linear everywhere now, so this pass is the encode. It used to be an
    // identity over the raster modes, which is why switching it off did nothing
    // to them and presented the traced mode's raw linear image as though it
    // were already display-referred.
    //
    // The dial gates the grade, not the encode. Ungraded still means a
    // correctly encoded picture - unit exposure, no tone curve - which is what
    // makes it a comparable diagnostic in all three modes.
    //
    // Retro is never graded, and that is a fact about its colour rather than a
    // preference. Its resolve computes in the original's display space - that
    // is the fidelity constraint of the mode - and inverts the result into
    // linear only so it can travel a linear pipeline. Those numbers are not
    // scene radiance, so an exposure stop and a tone curve have nothing to act
    // on: they would regrade a finished picture. Left alone, the encode here
    // cancels that inversion exactly and retro presents the frame it always
    // has. The same reasoning keeps SSAO and screen-space reflections out of
    // the mode. Delete the mode test to grade it, and it will look tonemapped,
    // because it will be.
    const bool grade = _options.grade && _options.mode != RenderMode::Retro;
    const bool displayReferred = (resolveFlags() & kResolveFlagDisplayReferred) != 0;
    PostProcessPushConstants push {grade ? 1u : 0u,
                                   displayReferred ? 1u : 0u,
                                   std::max(0.01f, _options.exposure),
                                   static_cast<uint32_t>(std::clamp(_options.tonemap, 0, 1))};
    tailPass(cmd, "postProcessFragment", globalsOffset, 0, &push, sizeof(push));
}

void ScenePipeline::debugViewPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::debugViewPass record");
    CommandBufferDebugScope debugScope(cmd, "debugViewPass");
    if (!_resolveMaterialSet) {
        // No merged geometry, so no material records to index and nothing to
        // report. Leave whatever the degenerate frame already published.
        return;
    }
    transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
    // Written through a storage descriptor, over the whole image: the view
    // replaces the shaded frame rather than compositing onto it.
    cmd.transitionImage(*_output, ImageLayout::General);

    PipelineKey key;
    key.module = "debug_view";
    key.computeEntry = "debugViewMain";
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;

    // Its own set rather than either resolve's: this pass wants the motion
    // target, which neither resolve binds, and wants none of the irradiance or
    // BRDF tables both of them do. Unit numbering is the resolves' own, which
    // the modules declare once each.
    //
    // The channel images stand where the shadow maps used to: the radiance
    // views read what the provider actually delivered instead of re-deriving
    // it. Bound only when they exist - retro allocates none, and its radiance
    // views paint the card off the flag bit rather than silently sampling the
    // stand-in textures every unbound unit falls back to, which once produced
    // a confident, stable, entirely fictional shadow term.
    std::vector<TextureBinding> sourceBindings {
        {1, &_gbuffer->color(GBufferAttachment::Diffuse)},
        {2, &_gbuffer->color(GBufferAttachment::EyeNormal)},
        {3, &_gbuffer->color(GBufferAttachment::Lightmap)},
        {4, &_gbuffer->color(GBufferAttachment::SelfIllum)},
        {5, &_gbuffer->depth()},
        {TextureUnits::gBufMotion, &_gbuffer->color(GBufferAttachment::Motion)},
        {TextureUnits::gBufTriangleId, &_gbuffer->color(GBufferAttachment::TriangleId)}};
    if (_channelImages[0][0]) {
        const int chFrame = _lastChannelFrame >= 0 ? _lastChannelFrame : 0;
        const auto &chan = _channelImages[chFrame];
        const auto channel = [&chan](ChannelSlot c) {
            return chan[static_cast<int>(c)].get();
        };
        cmd.transitionImages({channel(ChannelSlot::Diffuse),
                              channel(ChannelSlot::Specular),
                              channel(ChannelSlot::NoiseFree),
                              channel(ChannelSlot::DirectDiffuse)},
                             ImageLayout::ShaderRead);
        sourceBindings.push_back({TextureUnits::channelDiffuse, channel(ChannelSlot::Diffuse)});
        sourceBindings.push_back({TextureUnits::channelSpecular, channel(ChannelSlot::Specular)});
        sourceBindings.push_back({TextureUnits::channelNoiseFree, channel(ChannelSlot::NoiseFree)});
        sourceBindings.push_back({TextureUnits::channelDirect, channel(ChannelSlot::DirectDiffuse)});
    }
    auto sourceSet = _renderer.descriptors().acquireTextureDescriptorSet(
        _renderer.uniformRing().frame(), sourceBindings);

    auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
    cmd.bindComputePipeline(pipeline.pipeline);
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                 offsets.data(), static_cast<uint32_t>(offsets.size()));
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet,
                                 nullptr, 0);
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kMegaDrawSet,
                                 _resolveMaterialSet, nullptr, 0);
    cmd.bindComputeDescriptorSet(pipeline.layout, IDescriptors::kResolveSet,
                                 resolveSet(_output.get()), nullptr, 0);
    // Bit 8 marks channels-absent: retro allocates no channel images, so its
    // radiance views (8, 9, 11, 15) must paint the card rather than sample a
    // stand-in texture and report confident fiction.
    const bool channelsAbsent = !_channelImages[0][0];
    const DebugViewPushConstants push {
        static_cast<uint32_t>(std::clamp(_options.debugView, 0, kMaxDebugView)) |
            (channelsAbsent ? kDebugViewChannelsAbsent : 0u),
        std::max(0.05f, _options.exposure),
        static_cast<uint32_t>(std::clamp(_options.tonemap, 0, 1))};
    cmd.pushComputeConstants(pipeline.layout, &push, sizeof(push));
    cmd.dispatchCompute({(chainSize().x + kResolveGroupSize - 1) / kResolveGroupSize,
                         (chainSize().y + kResolveGroupSize - 1) / kResolveGroupSize, 1});

    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
}

void ScenePipeline::debugOverlayPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::debugOverlayPass record");
    CommandBufferDebugScope debugScope(cmd, "debugOverlayPass");
    if (_overlayShapes.empty() && _overlayLines.empty() && _overlayLabels.empty()) {
        return;
    }
    // Over the finished display-referred image, and depth is NOT an attachment
    // here: both stages sample the G-buffer depth themselves and drop the
    // opacity of whatever lies behind geometry instead of discarding it.
    cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
    cmd.transitionImage(*_output, ImageLayout::ColorAttachment);

    PipelineKey boxKey;
    boxKey.module = "debug_overlay";
    boxKey.vertexEntry = "overlayVertex";
    boxKey.fragmentEntry = "overlayFragment";
    boxKey.colorFormats = {_output->pixelFormat()};
    boxKey.blend = BlendMode::Normal;

    // The depth both stages test against, plus the atlas the labels sample.
    // One set serves both draws.
    std::vector<TextureBinding> sourceBindings {{TextureUnits::gBufDepth, &_gbuffer->depth()}};
    if (_overlayFont) {
        sourceBindings.push_back(
            {TextureUnits::mainTex, &_renderer.resources().get(_overlayFont->texture())});
    }
    const int frame = _renderer.uniformRing().frame();
    auto sourceSet = _renderer.descriptors().acquireTextureDescriptorSet(frame, sourceBindings);
    auto uniformSet = _renderer.descriptors().uniformDescriptorSet(frame);
    const glm::vec2 resolution {chainSize()};

    RenderAttachment color {_output->sampleView(), ImageLayout::ColorAttachment,
                            AttachmentLoad::Load, AttachmentStore::Store};
    cmd.beginRendering(chainSize(), {color}, nullptr, 0, true);

    if (!_overlayShapes.empty()) {
        PipelineBinding pipeline = _renderer.pipelines().get(boxKey);
        cmd.bindPipeline(pipeline.pipeline);
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet, nullptr, 0);
        // One draw per box: its eight corners ride the aabb block through the
        // uniform ring at a fresh dynamic offset, the same idiom the per-mesh
        // locals use, so there is no vertex buffer at all.
        for (const auto &shape : _overlayShapes) {
            AABBUniforms aabbUniforms;
            std::copy(std::begin(shape.corners), std::end(shape.corners), aabbUniforms.corners);
            std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
            offsets[UniformBlockBindingPoints::globals] = globalsOffset;
            offsets[UniformBlockBindingPoints::aabb] = _renderer.uniformRing().push(aabbUniforms);
            cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                  offsets.data(), static_cast<uint32_t>(offsets.size()));
            const DebugOverlayPushConstants push {shape.color, resolution,
                                                  kOverlayLineHalfWidth,
                                                  kOverlayOccludedLine, glm::vec3(0.0f)};
            cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
            // Twelve edges, one quad each, six vertices per quad.
            cmd.draw(72, 1);
        }
    }

    // The free lines, between the boxes and the labels. Same pipeline state and
    // same descriptor set as the boxes - only the vertex entry and the vertex
    // count differ - so a pathfinder edge and a bounding-box edge are the same
    // draw with different endpoints.
    if (!_overlayLines.empty()) {
        PipelineKey lineKey = boxKey;
        lineKey.vertexEntry = "overlayLineVertex";
        PipelineBinding pipeline = _renderer.pipelines().get(lineKey);
        cmd.bindPipeline(pipeline.pipeline);
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet, nullptr, 0);
        for (const auto &line : _overlayLines) {
            AABBUniforms aabbUniforms;
            // Only the first two slots are read; the rest of the block is along
            // for the ride so the box path's uniform plumbing serves both.
            aabbUniforms.corners[0] = line.start;
            aabbUniforms.corners[1] = line.end;
            std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
            offsets[UniformBlockBindingPoints::globals] = globalsOffset;
            offsets[UniformBlockBindingPoints::aabb] = _renderer.uniformRing().push(aabbUniforms);
            cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                  offsets.data(), static_cast<uint32_t>(offsets.size()));
            const DebugOverlayPushConstants push {line.color, resolution,
                                                  std::max(0.5f, line.halfWidth),
                                                  kOverlayOccludedLine, glm::vec3(0.0f)};
            cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
            // One quad, six vertices - against the box path's twelve quads.
            cmd.draw(6, 1);
        }
    }

    // The labels, in this same pass and against this same depth image, so text
    // and lines answer occlusion identically.
    if (_overlayFont && !_overlayLabels.empty()) {
        PipelineKey textKey = boxKey;
        textKey.vertexEntry = "overlayTextVertex";
        textKey.fragmentEntry = "overlayTextFragment";
        PipelineBinding pipeline = _renderer.pipelines().get(textKey);
        cmd.bindPipeline(pipeline.pipeline);
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet, nullptr, 0);

        PipelineKey backgroundKey = textKey;
        backgroundKey.vertexEntry = "overlayTextBackgroundVertex";
        backgroundKey.fragmentEntry = "overlayFragment";
        PipelineBinding backgroundPipeline = _renderer.pipelines().get(backgroundKey);

        for (const auto &label : _overlayLabels) {
            if (label.text.empty()) {
                continue;
            }
            // A label is one anchor and one or more lines: every line lays out
            // in SCREEN space under the first, so a multi-line label stays a
            // block however the camera moves, instead of separately projected
            // lines drifting through each other.
            const float lineHeight = _overlayFont->height() + 2.0f;

            // A dim backing behind the whole block first, so text reads over
            // wireframe as well as over scenery. Sized from the widest line;
            // the rect rides to the shader in the text block's first slot.
            {
                float maxWidth = 0.0f;
                int numLines = 0;
                std::string_view measuring = label.text;
                while (!measuring.empty()) {
                    const size_t split = measuring.find('\n');
                    const std::string_view line = measuring.substr(0, split);
                    measuring = split == std::string_view::npos ? std::string_view()
                                                                : measuring.substr(split + 1);
                    maxWidth = std::max(
                        maxWidth,
                        -2.0f * _overlayFont->textOffset(line, TextGravity::CenterBottom, 1.0f).x);
                    ++numLines;
                }
                constexpr float kPad = 3.0f;
                TextUniforms rect;
                rect.chars[0].posScale =
                    glm::vec4(-0.5f * maxWidth - kPad, -kPad, maxWidth + 2.0f * kPad,
                              numLines * lineHeight + 2.0f * kPad);
                cmd.bindPipeline(backgroundPipeline.pipeline);
                std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
                offsets[UniformBlockBindingPoints::globals] = globalsOffset;
                offsets[UniformBlockBindingPoints::text] = _renderer.uniformRing().push(rect);
                cmd.bindDescriptorSet(backgroundPipeline.layout, IDescriptors::kUniformSet,
                                      uniformSet, offsets.data(),
                                      static_cast<uint32_t>(offsets.size()));
                const DebugOverlayPushConstants push {
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.45f * label.color.a),
                    resolution, kOverlayLineHalfWidth, kOverlayOccludedLabel,
                    label.position};
                cmd.pushGraphicsConstants(backgroundPipeline.layout, &push, sizeof(push));
                cmd.draw(6, 1);
                cmd.bindPipeline(pipeline.pipeline);
            }

            std::string_view remaining = label.text;
            int lineIndex = 0;
            while (!remaining.empty()) {
                const size_t split = remaining.find('\n');
                const std::string_view line = remaining.substr(0, split);
                remaining = split == std::string_view::npos ? std::string_view()
                                                            : remaining.substr(split + 1);
                if (line.empty()) {
                    ++lineIndex;
                    continue;
                }
                const glm::vec2 origin =
                    _overlayFont->textOffset(line, TextGravity::CenterBottom, 1.0f) +
                    glm::vec2(0.0f, lineIndex * lineHeight);
                // Drawn the way renderDeveloperText draws it: a one-pixel black
                // copy under the colour, so a glyph does not vanish over pale
                // scenery. A shifted copy, never a dilation - dilating closes
                // the font's one-texel counters and turns a, o and 0 into one
                // block.
                for (int pass = 0; pass < 2; ++pass) {
                    const bool shadow = pass == 0;
                    TextUniforms chars;
                    const int numChars = fillTextRun(
                        *_overlayFont, line,
                        shadow ? origin + glm::vec2(1.0f) : origin, chars);
                    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
                    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
                    offsets[UniformBlockBindingPoints::text] = _renderer.uniformRing().push(chars);
                    cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                                          offsets.data(), static_cast<uint32_t>(offsets.size()));
                    const DebugOverlayPushConstants push {
                        shadow ? glm::vec4(0.0f, 0.0f, 0.0f, label.color.a) : label.color,
                        resolution, kOverlayLineHalfWidth, kOverlayOccludedLabel,
                        label.position};
                    cmd.pushGraphicsConstants(pipeline.layout, &push, sizeof(push));
                    cmd.draw(6, static_cast<uint32_t>(numChars));
                }
                ++lineIndex;
            }
        }
    }
    cmd.endRendering();
    cmd.transitionImage(*_output, ImageLayout::ShaderRead);
}

void ScenePipeline::fogPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::fogPass record");
    CommandBufferDebugScope debugScope(cmd, "fogPass");
    // Both switches: the player's, and whether the AREA authored any fog. With
    // the latter off the fog uniforms are untouched zeros, and reading a
    // density out of them would fog a module that has none.
    if (!_options.fog || !_fogEnabled) {
        return;
    }
    // Over the opaque image only, BEFORE transparency: this pass reads the
    // depth buffer, and a blended surface writes none, so it fogs itself in
    // its own shader at its own position instead - see sceneDrawBlendedFragment.
    const FogPushConstants push = fogParameters();
    tailPass(cmd, "fogFragment", globalsOffset, 0, &push, sizeof(push), true);
}

void ScenePipeline::sharpenPass(ICommandBuffer &cmd, uint32_t globalsOffset) {
    R_PROFILE_ZONE("ScenePipeline::sharpenPass record");
    CommandBufferDebugScope debugScope(cmd, "sharpenPass");
    // Last, after the display transform, because an unsharp mask is a
    // judgement about the picture a viewer sees rather than about scene
    // radiance: sharpening linear colour weights a highlight far above what it
    // looks like once the curve has compressed it. This is the arm of the one
    // sharpness dial that runs when no upscaler is in the slot to sharpen from
    // inside itself; RenderPipeline decides which arm that is.
    ScreenEffectUniforms screenEffect;
    screenEffect.screenResolution = glm::vec2(chainSize());
    screenEffect.screenResolutionRcp = 1.0f / glm::vec2(chainSize());
    screenEffect.sharpenAmount = std::max(0.0f, _options.sharpness);
    auto screenEffectOffset = _renderer.uniformRing().push(screenEffect);
    tailPass(cmd, "sharpenFragment", globalsOffset, screenEffectOffset, nullptr, 0);
}

Texture &ScenePipeline::render(const SceneFramePlan &plan,
                                     ISceneCallbacks &callbacks) {
    auto &cmd = _renderer.recordingCommandBuffer();
    // Give the render-resolution pair back to the chain. The upscale hands the
    // display pair over mid-frame and the tail finishes on it, so every frame
    // starts by undoing that - otherwise the geometry pass would find itself
    // rendering into display-sized attachments at render-sized extents.
    if (_chainAtDisplaySize) {
        std::swap(_output, _displayColor);
        std::swap(_tailColor, _displayTail);
        _chainAtDisplaySize = false;
    }
    _shadowCasters = plan.shadowCasters;
    _groundHeight = plan.groundHeight;
    _fogEnabled = plan.fogEnabled;
    _overlayShapes = plan.overlayShapes;
    _overlayLines = plan.overlayLines;
    _overlayLabels = plan.overlayLabels;
    _overlayFont = plan.overlayFont;
    _transparentOutput = plan.transparentOutput;
    _shadowCasterCategories = plan.shadowCasterCategories;
    _mergedScene = {};
    _mergedScenePrepared = false;
    // The scene graph stores the frame's Vulkan-native uniform values here;
    // copy them into this frame's arena. Clip-space y is still handled by the
    // flipped viewport so triangle winding remains unchanged.
    auto globals = _uniforms.globals();
    auto globalsOffset = _renderer.uniformRing().push(globals);
    _renderer.uniformRing().setGlobalsOffset(globalsOffset);

    if (_primaryRayMode) {
        // A traced sky is the same sky the coverage pass uses to decide that a
        // GUI pixel is occupied. Prepare it once before either consumer.
        _skyBinding = callbacks.prepareSky(cmd);
        // Primary visibility is recorded before the trace, and the tracer now
        // READS it: the kernel reconstructs its primary surface from these
        // attachments instead of tracing a camera ray to find the same surface
        // a second time. They are therefore published as sampled BEFORE the
        // trace rather than after it, which is where they used to be published
        // when they were a validation target the trace was kept blind to.
        for (const auto step : plan.steps) {
            if (step == SceneStep::Shadow) {
                shadowPass(cmd, globalsOffset, callbacks);
            } else if (step == SceneStep::Geometry) {
                geometryPass(cmd, globalsOffset, callbacks);
            }
        }
        transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
        cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
        cmd.transitionImage(*_output, ImageLayout::General);
        // Cleared before the trace fills it: runComposite stays false unless the
        // tracer asks for the composite, which is exactly when the trace kernel
        // did NOT already write the final image itself.
        _tracingOutput = {};
        callbacks.renderPrimary(
            {&cmd, globalsOffset, _output.get(), _mergedScene,
             globals.view, globals.projection, globals.jitter, _skyBinding,
             gbufferBinding(), acquireChannelBinding(), &_tracingOutput});
        // The composite the tracer used to run itself. It reads the channel
        // images this pipeline owns and the denoised pair the tracer handed
        // back, and overwrites _output with the assembled linear-HDR image.
        if (_tracingOutput.runComposite) {
            compositePass(cmd);
        }
        cmd.transitionImage(*_output, ImageLayout::ShaderRead);
        primaryCoveragePass(cmd, globalsOffset);
        // The common tail runs over the traced image exactly as it does over a
        // resolved one; the tracer stops at linear and the display transform
        // is the pass below, not the kernel.
        for (const auto step : plan.steps) {
            if (step == SceneStep::Blended) {
                // The traced tail runs the same transparency pass the raster
                // tail does, and for the same reason: additive layers - saber
                // blades, glow planes - are primary visibility that does not
                // fit in a G-buffer, and since the tracer stopped traversing
                // blended surfaces nothing else draws them.
                blendedPass(cmd, globalsOffset, callbacks);
            } else if (step == SceneStep::AntiAliasing) {
                antiAliasingPass(cmd, globalsOffset);
            } else if (step == SceneStep::Sharpen) {
                sharpenPass(cmd, globalsOffset);
            } else if (step == SceneStep::Bloom) {
                bloomPass(cmd, globalsOffset);
            } else if (step == SceneStep::PostProcess) {
                postProcessPass(cmd, globalsOffset);
            } else if (step == SceneStep::DebugView) {
                debugViewPass(cmd, globalsOffset);
            } else if (step == SceneStep::Fog) {
                fogPass(cmd, globalsOffset);
            } else if (step == SceneStep::DebugOverlay) {
                debugOverlayPass(cmd, globalsOffset);
            }
        }
        // The traced image is sampleable by now, so the preview can read it
        // like any other target. Without this the window would offer a target
        // it never draws, which is only marginally better than crashing.
        coveragePass(cmd, globalsOffset);
        previewPass(cmd, globalsOffset, callbacks);
        _renderer.resources().registerExternal(*_outputHandle, *_output);
        return *_outputHandle;
    }

    // Outside any pass, and before the resolve that reads it: a first bake of a
    // room records six cube-face passes of its own, which cannot happen inside
    // one. The bake pushes its own per-face globals through the ring and leaves
    // the last face's offset latched, so this frame's offset is restored
    // afterwards - the resolve binds it a few lines later.
    _skyBinding = callbacks.prepareSky(cmd);
    _renderer.uniformRing().setGlobalsOffset(globalsOffset);

    bool outputResolved = false;
    for (const auto step : plan.steps) {
        switch (step) {
        case SceneStep::ProcessPBRTextures:
            _renderer.pbrTextures().process(_renderer.recordingCommandBuffer(), globalsOffset);
            break;
        case SceneStep::Shadow:
            shadowPass(cmd, globalsOffset, callbacks);
            break;
        case SceneStep::Geometry:
            geometryPass(cmd, globalsOffset, callbacks);
            break;
        case SceneStep::PBRChannels:
            pbrChannelsPass(cmd, globalsOffset);
            break;
        case SceneStep::Composite:
            // The shared assembly over the channels the pass above shaded.
            // When there was nothing to shade, that pass cleared the output
            // itself and left runComposite false.
            if (_tracingOutput.runComposite) {
                cmd.transitionImage(*_output, ImageLayout::General);
                compositePass(cmd);
                cmd.transitionImage(*_output, ImageLayout::ShaderRead);
                // R14: transparent output takes its alpha from primary
                // coverage, exactly as the traced branch does; the pass
                // stands aside when the output is opaque.
                primaryCoveragePass(cmd, globalsOffset);
            }
            outputResolved = true;
            break;
        case SceneStep::Blended:
            blendedPass(cmd, globalsOffset, callbacks);
            break;
        case SceneStep::RetroResolve:
            retroResolvePass(cmd, globalsOffset);
            outputResolved = true;
            break;
        case SceneStep::ScreenSpaceReflections:
            screenSpaceReflectionPass(cmd, globalsOffset);
            break;
        case SceneStep::AntiAliasing:
            antiAliasingPass(cmd, globalsOffset);
            break;
        case SceneStep::Sharpen:
            sharpenPass(cmd, globalsOffset);
            break;
        case SceneStep::Bloom:
            bloomPass(cmd, globalsOffset);
            break;
        case SceneStep::PostProcess:
            postProcessPass(cmd, globalsOffset);
            break;
        case SceneStep::DebugView:
            debugViewPass(cmd, globalsOffset);
            break;
        case SceneStep::Fog:
            fogPass(cmd, globalsOffset);
            break;
        case SceneStep::DebugOverlay:
            debugOverlayPass(cmd, globalsOffset);
            break;
        }
    }

    // An empty raster plan is a supported degenerate frame. Keep the output
    // alive as a regular attachment, clear it to black, then publish it in the
    // layout the 2D compositor samples.
    if (!outputResolved) {
        cmd.transitionImage(*_output, ImageLayout::ColorAttachment);
        {
            RenderAttachment color {_output->sampleView(), ImageLayout::ColorAttachment,
                                    AttachmentLoad::Clear, AttachmentStore::Store};
            color.clear.color = {0.0f, 0.0f, 0.0f, _transparentOutput ? 0.0f : 1.0f};
            cmd.beginRendering(chainSize(), {color}, nullptr, 0, false);
            cmd.endRendering();
        }
        cmd.transitionImage(*_output, ImageLayout::ShaderRead);
        transitionGBuffer(cmd, *_gbuffer, ImageLayout::ShaderRead);
        cmd.transitionImage(_gbuffer->depth(), ImageLayout::DepthRead);
    }

    coveragePass(cmd, globalsOffset);
    previewPass(cmd, globalsOffset, callbacks);

    _renderer.resources().registerExternal(*_outputHandle, *_output);
    return *_outputHandle;
}

/**
 * How a target's format comes back on the CPU: channel count and element type.
 *
 * Read back as stored. Half-float targets are widened to float on the way out
 * rather than written as halves, so a dump from either backend has the same
 * dtype and the two can be subtracted without a cast - OpenGL's readback widens
 * them in the driver, and half to float is exact either way.
 */
struct DumpFormat {
    int channels;
    NpyType type;
    bool halfToFloat;
};

static std::optional<DumpFormat> dumpFormatFor(Format format) {
    switch (format) {
    case Format::R8G8B8A8Unorm:
    case Format::B8G8R8A8Unorm:
    case Format::B8G8R8A8Srgb:
        return DumpFormat {4, NpyType::UInt8, false};
    case Format::R8Unorm:
        return DumpFormat {1, NpyType::UInt8, false};
    case Format::R32Uint:
        return DumpFormat {1, NpyType::UInt32, false};
    case Format::R16Sfloat:
        return DumpFormat {1, NpyType::Float32, true};
    case Format::R16G16Sfloat:
        return DumpFormat {2, NpyType::Float32, true};
    case Format::R16G16B16A16Sfloat:
        return DumpFormat {4, NpyType::Float32, true};
    case Format::R32Sfloat:
    case Format::D32Sfloat:
        return DumpFormat {1, NpyType::Float32, false};
    default:
        return std::nullopt;
    }
}

/** IEEE half to float. Exact - every half has an exact float representation. */
static float halfToFloat(uint16_t half) {
    uint32_t sign = static_cast<uint32_t>(half & 0x8000) << 16;
    uint32_t exponent = (half >> 10) & 0x1f;
    uint32_t mantissa = half & 0x3ff;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: renormalise into float's wider exponent range.
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3ff;
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1f) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

/** Whether a format stores blue first, and so needs swizzling on the way out. */
static bool isBGRA(Format format) {
    switch (format) {
    case Format::B8G8R8A8Unorm:
    case Format::B8G8R8A8Srgb:
        return true;
    default:
        return false;
    }
}

std::vector<ScenePipeline::Target> ScenePipeline::targetEntries(
    const ISceneCallbacks &callbacks) const {
    if (!_inited) {
        return {};
    }
    std::vector<Target> entries;
    if (_primaryRayMode) {
        entries.push_back({"Traced output", "traced_output", TargetKind::Color,
                           _output.get(), ImageLayout::ShaderRead, false});
    }
    if (_channelImages[0][0]) {
        // The channel split behind the image, owned here now, and no longer the
        // traced mode's alone - the PBR channels pass fills the same images.
        // Without these a frame can only be judged as a whole, which cannot
        // separate a noisy channel from a composite that mis-assembles it. They
        // live in GENERAL: the providers and the composite read and write them
        // as storage images and nothing transitions them afterwards. Order and
        // names follow the trace kernel's aux order (tracing/outputs.slang).
        static const char *kChannelNames[kNumTracingChannels] {
            "Channel diffuse radiance", "Channel specular radiance", "Channel normal/roughness",
            "Channel viewZ", "Channel NRD motion", "Channel noise-free", "Channel diffuse factor",
            "Channel device depth", "Channel screen motion", "Channel specular factor",
            "Channel diffuse", "Channel eye normal", "Channel depth", "Channel motion",
            "Channel direct diffuse"};
        static const char *kChannelDumpNames[kNumTracingChannels] {
            "channel_radiance_diffuse", "channel_radiance_specular", "channel_normal_roughness",
            "channel_view_z", "channel_nrd_motion", "channel_noise_free", "channel_diff_factor",
            "channel_device_depth", "channel_screen_motion", "channel_spec_factor",
            "channel_diffuse", "channel_eye_normal", "channel_depth", "channel_motion",
            "channel_direct_diffuse"};
        const int frame = _lastChannelFrame >= 0 ? _lastChannelFrame : 0;
        for (int i = 0; i < kNumTracingChannels; ++i) {
            if (_channelImages[frame][i]) {
                entries.push_back({kChannelNames[i], kChannelDumpNames[i], TargetKind::Color,
                                   _channelImages[frame][i].get(), ImageLayout::General, false});
            }
        }
        // The denoiser's own outputs and the shadow-filter target still come
        // from the tracer, which owns them.
        for (const auto &channel : callbacks.primaryTargets()) {
            entries.push_back({channel.name, channel.dumpName, TargetKind::Color,
                               channel.image, ImageLayout::General, false});
        }
    }
    static const char *kDisplayNames[kGBufferAttachments.size()] = {
        "G-buffer diffuse", "G-buffer eye normal", "G-buffer lightmap",
        "G-buffer self-illum", "G-buffer motion", "G-buffer triangle ID"};
    static const char *kDumpNames[kGBufferAttachments.size()] = {
        "g_buffer_diffuse", "g_buffer_eye_normal", "g_buffer_lightmap",
        "g_buffer_self_illum", "g_buffer_motion", "g_buffer_triangle_id"};
    for (size_t i = 0; i < kGBufferAttachments.size(); ++i) {
        auto attachment = kGBufferAttachments[i];
        auto kind = attachment == GBufferAttachment::EyeNormal ? TargetKind::EyeNormal : attachment == GBufferAttachment::Motion ? TargetKind::Motion
                                                                                                                        : TargetKind::Color;
        entries.push_back({kDisplayNames[i], kDumpNames[i], kind, &_gbuffer->color(attachment),
                           ImageLayout::ShaderRead, false});
    }
    entries.push_back({"G-buffer depth", "g_buffer_depth", TargetKind::Depth,
                       &_gbuffer->depth(), ImageLayout::DepthRead, true});
    if (!_primaryRayMode) {
        entries.push_back({"Output", "output", TargetKind::Color,
                           _output.get(), ImageLayout::ShaderRead, false});
    }
    return entries;
}

void *ScenePipeline::renderTargetPreview(const std::string &name, int mode, float scale,
                                               const ISceneCallbacks &callbacks) {
    auto entries = targetEntries(callbacks);
    if (std::none_of(entries.begin(), entries.end(), [&name](const auto &entry) {
            return entry.name == name;
        })) {
        return nullptr;
    }
    if (!_preview) {
        _preview = std::make_unique<Preview>();
        _preview->image = _renderer.resources().makeImage();
        _preview->image->initColorAttachment({480, 360}, Format::R8G8B8A8Unorm);
        _preview->image->setSampler(_renderer.resources().sampler(
            getTextureProperties(TextureUsage::ColorBuffer)));
        _renderer.immediateSubmit([this](ICommandBuffer &cmd) {
            cmd.transitionImage(*_preview->image, ImageLayout::ShaderRead);
        });
        _preview->imguiTexture = _renderer.addPreviewTexture(*_preview->image);
    }
    _preview->target = name;
    _preview->mode = mode;
    _preview->scale = scale;
    return _preview->imguiTexture;
}

void ScenePipeline::previewPass(ICommandBuffer &cmd, uint32_t globalsOffset,
                                      const ISceneCallbacks &callbacks) {
    R_PROFILE_ZONE("ScenePipeline::previewPass record");
    CommandBufferDebugScope debugScope(cmd, "previewPass");
    if (!_preview) {
        return;
    }
    auto entries = targetEntries(callbacks);
    auto selected = std::find_if(entries.begin(), entries.end(), [this](const auto &entry) {
        return entry.name == _preview->target;
    });
    if (selected == entries.end()) {
        return;
    }

    cmd.transitionImage(*_preview->image, ImageLayout::ColorAttachment);

    PipelineKey key;
    key.module = kPostProcessModule;
    key.vertexEntry = "postVertex";
    key.fragmentEntry = "debugTextureFragment";
    key.colorFormats = {_preview->image->pixelFormat()};
    PipelineBinding pipeline = _renderer.pipelines().get(key);

    ScreenEffectUniforms screenEffect;
    screenEffect.clipNear = _uniforms.globals().clipNear;
    screenEffect.clipFar = _uniforms.globals().clipFar;
    // These fields are otherwise irrelevant to this pass and avoid another
    // uniform block solely for the two viewer controls.
    screenEffect.ssaoSampleRadius = static_cast<float>(_preview->mode);
    screenEffect.ssrBias = _preview->scale;
    auto screenEffectOffset = _renderer.uniformRing().push(screenEffect);
    std::array<uint32_t, IDescriptors::kNumUniformBlocks> offsets {};
    offsets[UniformBlockBindingPoints::globals] = globalsOffset;
    offsets[UniformBlockBindingPoints::screenEffect] = screenEffectOffset;
    auto sourceSet = _renderer.descriptors().acquireTextureDescriptorSet(
        _renderer.uniformRing().frame(), selected->image);
    {
        RenderAttachment color {_preview->image->sampleView(), ImageLayout::ColorAttachment,
                                AttachmentLoad::DontCare, AttachmentStore::Store};
        cmd.beginRendering({480, 360}, {color}, nullptr, 0, false);
        cmd.bindPipeline(pipeline.pipeline);
        auto uniformSet = _renderer.descriptors().uniformDescriptorSet(_renderer.uniformRing().frame());
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kUniformSet, uniformSet,
                              offsets.data(), static_cast<uint32_t>(offsets.size()));
        cmd.bindDescriptorSet(pipeline.layout, IDescriptors::kTextureSet, sourceSet, nullptr, 0);
        cmd.draw(3, 1);
        cmd.endRendering();
    }
    cmd.transitionImage(*_preview->image, ImageLayout::ShaderRead);
}

void ScenePipeline::dumpTargets(const std::filesystem::path &dir,
                                      const ISceneCallbacks &callbacks) {
    if (!_inited) {
        return;
    }
    std::filesystem::create_directories(dir);

    auto entries = targetEntries(callbacks);

    for (const auto &entry : entries) {
        auto format = dumpFormatFor(entry.image->pixelFormat());
        if (!format) {
            warn("Cannot dump target '" + std::string(entry.name) + "': unsupported format",
                 LogChannel::Graphics);
            continue;
        }
        auto raw = entry.image->readBack(entry.depth);
        auto extent = entry.image->extent();
        // Kept for any target whose format stores blue first. The scene output
        // no longer is one - it is RGBA float in every mode now, where it used
        // to inherit the swapchain's BGRA in the raster modes - but a dump
        // exists to be compared against another backend, so it is written in
        // one channel order rather than leaving whoever reads it to know which
        // target is which. Getting that wrong once already turned an 0.9
        // difference into an apparent 11.7 and invented a colour cast that was
        // not there.
        if (isBGRA(entry.image->pixelFormat())) {
            for (size_t i = 0; i + 3 < raw.size(); i += 4) {
                std::swap(raw[i], raw[i + 2]);
            }
        }
        const std::string_view dumpName(entry.dumpName);
        if (dumpName == "g_buffer_depth" && entry.depth) {
            // A depth attachment is a projective device-depth value. Dumps
            // compare scene representations, so publish positive linear
            // view-space distance in world units, matching the traced target.
            const float near = _uniforms.globals().clipNear;
            const float far = _uniforms.globals().clipFar;
            const size_t count = raw.size() / sizeof(float);
            std::vector<float> linearDepth(count);
            for (size_t i = 0; i < count; ++i) {
                float deviceDepth;
                std::memcpy(&deviceDepth, raw.data() + i * sizeof(float), sizeof(deviceDepth));
                // Scene projections are built in Vulkan's [0,1] depth range,
                // so this is the Vulkan form, not OpenGL's 2*n*f denominator.
                linearDepth[i] = near * far /
                                 std::max(far - deviceDepth * (far - near), 1e-6f);
            }
            writeNpy(dir / "g_buffer_depth.npy", linearDepth.data(), extent.x, extent.y, 1,
                     NpyType::Float32);
            continue;
        }
        const bool yCoCgRadiance = dumpName == "traced_radiance_diffuse" ||
                                   dumpName == "traced_radiance_specular" ||
                                   dumpName == "denoised_diffuse" ||
                                   dumpName == "denoised_specular";
        auto path = dir / (std::string(entry.dumpName) + ".npy");
        if (format->halfToFloat) {
            size_t count = raw.size() / sizeof(uint16_t);
            std::vector<float> widened(count);
            for (size_t i = 0; i < count; ++i) {
                uint16_t half;
                std::memcpy(&half, raw.data() + i * sizeof(uint16_t), sizeof(half));
                widened[i] = halfToFloat(half);
            }
            // NRD consumes its radiance targets in YCoCg, but diagnostics use
            // one colour space across every dumped target.
            if (yCoCgRadiance) {
                for (size_t i = 0; i + 3 < widened.size(); i += 4) {
                    const float y = widened[i];
                    const float co = widened[i + 1];
                    const float cg = widened[i + 2];
                    const float t = y - cg * 0.5f;
                    widened[i] = std::max(t - co * 0.5f + co, 0.0f);
                    widened[i + 1] = std::max(cg + t, 0.0f);
                    widened[i + 2] = std::max(t - co * 0.5f, 0.0f);
                }
            }
            writeNpy(path, widened.data(), extent.x, extent.y, format->channels, format->type);
        } else {
            writeNpy(path, raw.data(), extent.x, extent.y, format->channels, format->type);
        }
    }
    if (_options.mode != RenderMode::Retro) {
        // Cube arrays are unrolled face-after-face: layer 0 +X..-Z, then
        // layer 1 +X..-Z, and so on. Keeping every layer makes the dump useful
        // even when a scene derives more than one environment map.
        auto dumpCubeArray = [&dir](const char *name, const IImage &image, int mip,
                                     uint32_t layers) {
            constexpr uint32_t kFaces = 6;
            auto format = dumpFormatFor(image.pixelFormat());
            if (!format) {
                warn("Cannot dump cube array '" + std::string(name) + "': unsupported format",
                     LogChannel::Graphics);
                return;
            }
            auto raw = image.readBack(mip, layers);
            auto extent = glm::max(glm::ivec2(1), image.extent() >> mip);
            // Vulkan's image-copy rows are upside down relative to the GL
            // cube-array readback. Normalize the diagnostic layout here; this
            // does not affect the texture's sampling convention.
            std::vector<uint8_t> flipped(raw.size());
            size_t rowBytes = raw.size() / (static_cast<size_t>(layers) * extent.y);
            size_t faceBytes = rowBytes * extent.y;
            for (uint32_t face = 0; face < layers; ++face) {
                for (int y = 0; y < extent.y; ++y) {
                    std::memcpy(flipped.data() + face * faceBytes + y * rowBytes,
                                raw.data() + face * faceBytes + (extent.y - 1 - y) * rowBytes,
                                rowBytes);
                }
            }
            raw = std::move(flipped);
            auto path = dir / (std::string(name) + ".npy");
            if (format->halfToFloat) {
                size_t count = raw.size() / sizeof(uint16_t);
                std::vector<float> widened(count);
                for (size_t i = 0; i < count; ++i) {
                    uint16_t half;
                    std::memcpy(&half, raw.data() + i * sizeof(uint16_t), sizeof(half));
                    widened[i] = halfToFloat(half);
                }
                writeNpy(path, widened.data(), extent.x, extent.y * static_cast<int>(layers),
                         format->channels, format->type);
            } else {
                writeNpy(path, raw.data(), extent.x, extent.y * static_cast<int>(layers),
                         format->channels, format->type);
            }
        };
        auto &pbr = _renderer.pbrTextures();
        auto &irradiance = pbr.irradianceArray();
        auto &prefiltered = pbr.prefilteredArray();
        dumpCubeArray("irradiance_map_array", irradiance, 0, 16 * 6);
        for (int mip = 0; mip < prefiltered.mipLevels(); ++mip) {
            dumpCubeArray(("prefiltered_env_map_array_mip" + std::to_string(mip)).c_str(),
                          prefiltered, mip, 16 * 6);
        }

        auto dumpSourceEnvMap = [&dir, this](int layer, const Texture &texture) {
            if (!texture.is2D() && !texture.isCubeMap()) {
                warn("Cannot dump environment source '" + texture.name() +
                         "': unsupported texture shape",
                     LogChannel::Graphics);
                return;
            }
            const auto &image = _renderer.resources().get(texture);
            auto format = dumpFormatFor(image.pixelFormat());
            bool compressed = image.pixelFormat() == Format::BC1RGBAUnormBlock ||
                              image.pixelFormat() == Format::BC3UnormBlock;
            // The OpenGL counterpart explicitly widens every source to RGBA8.
            // Decode BC sources to that same layout before writing the dump.
            if ((!format || format->channels != 4 || format->type != NpyType::UInt8) && !compressed) {
                warn("Cannot dump environment source '" + texture.name() +
                         "': unsupported Vulkan format",
                     LogChannel::Graphics);
                return;
            }
            uint32_t layers = texture.isCubeMap() ? kNumCubeFaces : 1;
            for (int mip = 0; mip < image.mipLevels(); ++mip) {
                auto raw = image.readBack(mip, layers);
                auto extent = glm::max(glm::ivec2(1), image.extent() >> mip);
                if (compressed) {
                    size_t blockBytes = image.pixelFormat() == Format::BC1RGBAUnormBlock ? 8 : 16;
                    size_t faceBytes = static_cast<size_t>((extent.x + 3) / 4) *
                                       ((extent.y + 3) / 4) * blockBytes;
                    std::vector<uint8_t> decoded(static_cast<size_t>(extent.x) * extent.y * layers * 4);
                    std::vector<uint32_t> pixels(static_cast<size_t>(extent.x) * extent.y);
                    for (uint32_t face = 0; face < layers; ++face) {
                        if (image.pixelFormat() == Format::BC1RGBAUnormBlock) {
                            decompressDXT1(extent.x, extent.y, raw.data() + face * faceBytes,
                                           pixels.data());
                        } else {
                            decompressDXT5(extent.x, extent.y, raw.data() + face * faceBytes,
                                           pixels.data());
                        }
                        for (size_t i = 0; i < pixels.size(); ++i) {
                            auto pixel = pixels[i];
                            auto *dst = decoded.data() + (static_cast<size_t>(face) * pixels.size() + i) * 4;
                            dst[0] = (pixel >> 24) & 0xff;
                            dst[1] = (pixel >> 16) & 0xff;
                            dst[2] = (pixel >> 8) & 0xff;
                            dst[3] = image.pixelFormat() == Format::BC1RGBAUnormBlock ? 0xff : pixel & 0xff;
                        }
                    }
                    raw = std::move(decoded);
                }
                std::vector<uint8_t> flipped(raw.size());
                size_t rowBytes = raw.size() / (static_cast<size_t>(layers) * extent.y);
                size_t faceBytes = rowBytes * extent.y;
                for (uint32_t face = 0; face < layers; ++face) {
                    for (int y = 0; y < extent.y; ++y) {
                        std::memcpy(flipped.data() + face * faceBytes + y * rowBytes,
                                    raw.data() + face * faceBytes + (extent.y - 1 - y) * rowBytes,
                                    rowBytes);
                    }
                }
                auto name = "environment_map_layer" + std::to_string(layer) +
                            "_mip" + std::to_string(mip);
                writeNpy(dir / (name + ".npy"), flipped.data(), extent.x,
                         extent.y * static_cast<int>(layers), 4, NpyType::UInt8);
            }
        };
        for (const auto &[layer, texture] : pbr.sourceEnvMaps()) {
            dumpSourceEnvMap(layer, *texture);
        }
    }
    info("Dumped " + std::to_string(entries.size()) + " render targets to " + dir.string(),
         LogChannel::Graphics);
}

std::vector<TargetInfo> ScenePipeline::targets(
    const ISceneCallbacks &callbacks) const {
    std::vector<TargetInfo> result;
    for (const auto &entry : targetEntries(callbacks)) {
        result.push_back({entry.name, entry.kind});
    }
    return result;
}

} // namespace graphics

} // namespace reone
