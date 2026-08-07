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

#pragma once

#include "types.h"

namespace reone {

namespace graphics {

/**
 * What occupies the common anti-aliasing slot, between transparency and the
 * display transform.
 *
 * A slot rather than a switch: every mode runs the same one, so a new resolve
 * is a new case here and a new branch in the pass, not another place in the
 * chain. One slot means one occupant - running a temporal resolve and then a
 * spatial one over its output is two anti-aliasers stacked, which is what this
 * enum exists to make unrepresentable.
 */
enum class AntiAliasing {
    None,
    Fxaa,
    /**
     * FidelityFX Super Resolution 2 at NativeAA (1.0x): a temporal resolve
     * over the G-buffer's depth and motion. Available in every mode because
     * primary visibility is rasterized in every mode. Selecting it changes
     * what the pipeline allocates, so it is applied on a graphics rebuild
     * rather than on the next frame.
     */
    Fsr,
};

/**
 * Which NRD denoiser resolves the traced channels.
 *
 * The pair are not interchangeable at the input: REBLUR takes radiance in YCoCg
 * with the hit distance normalized against its own curve, RELAX takes linear
 * radiance and a raw world-space distance. The trace kernel therefore has to
 * know which one it is writing for, so this value reaches the shader rather
 * than staying on the CPU.
 */
/**
 * Highest debug channel, matching the kDebug* numbering in
 * slang/debug_view.slang. Channels above 14 are produced by the tracer or its
 * resolve and have no counterpart in the raster modes.
 */
constexpr int kMaxDebugView = 19;

/** Channels the post-denoise resolve produces, rather than the trace kernel. */
constexpr bool isResolveDebugView(int view) {
    return view >= 17 && view <= 19;
}

enum class Denoiser {
    /** Cheaper, and spends its budget on spatial filtering. */
    Reblur,
    /** An a-trous edge-stopping filter: keeps edges and gloss, costs more. */
    Relax,
};

/**
 * Which renderer shades the frame.
 *
 * One option with three values rather than two options that had to be read
 * together. It maps 1:1 onto what the pipeline factory takes, so nothing
 * downstream reconstructs it from a string and a bool - which is exactly where
 * a mode could be, and once was, silently ignored.
 *
 * The values do not share a cost of changing. Retro and PBR pick a resolve step
 * per frame over targets a raster pipeline has already allocated, so moving
 * between them is live; path tracing decides whether a ray-query pipeline
 * exists at all and what format the scene output carries, so crossing into or
 * out of it needs a rebuild. See graphicsOptionApply.
 */
enum class RenderMode {
    /** The original's lighting model, kept as a reference rather than improved. */
    Retro,
    PBR,
    PathTracing,
};

struct GraphicsOptions {
    int width {1024};
    int height {768};
    int winScale {100};
    bool fullscreen {false};
    /**
     * Never show the window. For scripted batch runs - a warp loop launching
     * one process per module must not pop a window on the desktop every few
     * seconds. Rendering and capture work as usual; only presentation goes to
     * a hidden surface.
     */
    bool headless {false};
    bool vsync {true};
    bool grass {true};
    /** Multiplier on the area's authored Grass_Density, so areas keep their variation. */
    float grassDensity {1.0f};
    /**
     * The renderer, as one three-way choice. "raster" is still accepted as an
     * input spelling for Retro so existing scripts and configs keep working;
     * see optionsregistry.cpp.
     */
    RenderMode mode {RenderMode::PBR};
    /**
     * Hash the GpuScene upload every frame so --dumptargets can log it. The
     * hash walks every uploaded byte, which is measurable CPU per frame, so
     * it is on only when a dump was requested - the one consumer it has.
     */
    bool hashUploads {false};
    /** Rebuild a second CPU scene every frame and compare its upload. */
    bool admissionShadow {false};
    /** Render through the full CPU collection/classification path. */
    bool admissionForceFull {false};

    // Path tracing

    /**
     * Hemisphere samples per pixel for the second bounce.
     *
     * Cost is very close to linear - measured on a 5090 at 1920x1080 in
     * danm14ab, 1 sample costs 4.9 ms/frame and 16 costs 38.8. Noise falls as
     * the square root, so doubling this halves neither. Eight is the point
     * where the image reads clearly while the frame still moves.
     */
    int pathTracingSamples {3};

    /**
     * Light-balance knobs. They scale sources, not the image: sky is authored
     * background geometry (semantic, never color-based), emissive is every
     * other emitter, and lightmap scales the baked radiance cache that stands
     * in for light sources the tracer cannot see yet.
     *
     * These describe the scene's light rather than a renderer's treatment of
     * it; the tracer is only their consumer today. Sky loses the prefix
     * because the sky bake is common infrastructure - the raster composite
     * takes the same cube, at texel identity, without scaling it.
     */
    /** Calibration session of 2026-07-29: sky and emissive at 2.5, the
        lightmap cache retired to zero - two bounces of real transport
        replace it - and the sun at 2.5 where the Dantooine dusk reads as a
        sun. All still dials; these are the graded defaults. */
    float skyIntensity {2.5f};
    float ptEmissiveIntensity {2.5f};
    float ptLightmapIntensity {0.0f};
    float ptDirectIntensity {1.0f};
    float ptSunIntensity {2.5f};
    /** Path depth after the primary hit. */
    int ptBounces {2};
    /** Secondary-ray origin offset along the geometric normal, world units. */
    float ptRayOffset {0.01f};
    /** GPU trace-stats counters; off by default, the atomics cost frame time. */
    bool ptTraceStats {false};
    /**
     * REBLUR through NRD, when the build carries it (ENABLE_NRD - a local
     * developer toggle for license reasons). On by default there: the whole
     * point of the split. Without NRD in the build the dial is inert.
     */
    bool ptDenoise {true};
    /**
     * Direct light at the primary vertex bypasses the denoiser and is applied
     * at the resolve.
     *
     * A denoiser is built for indirect light: smooth, slow, and worth a wide
     * spatial kernel. A shadow edge is the opposite, and the kernel that
     * settles bounce noise is the kernel that softens contact. This channel is
     * stratified where it is sampled - a dithered, low-discrepancy point on the
     * light - and whatever noise is left is the temporal resolve's to take.
     *
     * Off restores the previous routing, through the denoiser, for comparison.
     */
    bool ptDirectChannel {true};
    /**
     * Filter the direct channel with a radius derived from the penumbra the
     * geometry implies, rather than a fixed one.
     *
     * The tracer records how far away whatever blocked each shadow ray was; a
     * source of known angular size at that distance implies a penumbra of a
     * particular width, and that width in pixels is the only radius that blurs
     * a soft shadow by as much as it is soft while leaving a contact edge
     * untouched. It is what a general denoiser cannot know.
     *
     * Off by default, because with a temporal resolver in the slot it makes the
     * picture worse rather than better. Blue noise is built so that its error
     * sits at high spatial frequency, which is exactly the part a temporal
     * resolve averages away and the part FSR's neighbourhood clamp is willing
     * to reject. A spatial blur moves that error down into low frequency, and a
     * low-frequency blob that changes between frames is indistinguishable from
     * signal to a clamp - so it survives, and then it boils.
     *
     * Measured over 32 FSR frames on the region the filter touches: mean
     * frame-to-frame luma delta 0.7429 with it off, 0.7584 at the physical
     * radius, 0.7507 forced to 8 pixels. Every filtered variant is less stable
     * than none, and none of them is quieter. It stays available for the case
     * with no temporal resolver, where nothing else is averaging.
     */
    bool ptShadowFilter {false};
    /** Ceiling on that radius in pixels, whatever the geometry asks for. */
    float ptShadowFilterMaxRadius {24.0f};
    /**
     * Multiplier on the radius the geometry implies. 1 is the physical answer;
     * above it trades penumbra fidelity for a quieter shadow.
     */
    float ptShadowFilterRadiusScale {1.0f};
    /**
     * Floor on the radius, in pixels, applied only where something actually
     * blocked the light. Unphysical by construction: it exists because the
     * residual noise in this channel is not penumbra-scale, and a filter sized
     * strictly by the geometry will not touch it. Zero leaves the estimate
     * alone; a contact edge stays sharp either way, since an unshadowed pixel
     * has no penumbra for the floor to apply to.
     */
    float ptShadowFilterMinRadius {0.0f};
    /** Relative view-depth difference a tap may have before it is rejected. */
    float ptShadowFilterDepthTolerance {0.02f};
    /** Minimum normal agreement a tap may have before it is rejected. */
    float ptShadowFilterNormalTolerance {0.9f};
    /**
     * Which NRD denoiser runs. REBLUR is the cheaper, blurrier one and was the
     * only choice here; RELAX keeps edges and specular detail at a higher cost,
     * which is the trade this content wants. Staged, not live: NRD fixes the
     * denoiser when the instance is built.
     */
    Denoiser ptDenoiser {Denoiser::Relax};
    /**
     * Denoiser tuning, exposed in the Path tracing panel. Accumulation is in
     * seconds, which is what NRD asks to be configured in - a frame count is
     * only its internal unit, and holding one fixed is why the picture got
     * worse as the frame rate rose.
     *
     * The rejection fractions are back at NRD's own values. The graded set they
     * replace ran roughly five times wider - lobe angle 0.77 against 0.15,
     * plane distance 0.099 against 0.02 - which reuses history across normals
     * and depths that do not belong together, and is what took the contact
     * shadows and the sharp folds with it.
     */
    float ptNrdAccumulationTime {0.5f};
    float ptNrdFastAccumulationTime {0.1f};
    float ptNrdStabilizationTime {0.0f};
    int ptNrdHistoryFixFrames {3};
    float ptNrdDiffusePrepassBlurRadius {30.0f};
    float ptNrdSpecularPrepassBlurRadius {50.0f};
    float ptNrdLobeAngleFraction {0.15f};
    float ptNrdRoughnessFraction {0.15f};
    float ptNrdDisocclusionThreshold {0.01f};
    bool ptNrdAntiFirefly {true};
    /** REBLUR only. */
    float ptNrdMinBlurRadius {1.0f};
    float ptNrdMaxBlurRadius {30.0f};
    float ptNrdPlaneDistanceSensitivity {0.02f};
    /** RELAX only. */
    int ptNrdAtrousIterations {5};
    float ptNrdDiffusePhiLuminance {2.0f};
    float ptNrdSpecularPhiLuminance {1.0f};
    float ptNrdDepthThreshold {0.003f};
    float ptNrdSpecularLobeAngleSlack {0.15f};
    /**
     * The debug channel view: 0 off, then the numbering in
     * slang/debug_view.slang.
     *
     * Not tracer-only, which is why it has lost the pt prefix. Almost every
     * channel is a G-buffer quantity, and every mode rasterizes that G-buffer,
     * so the selection is honoured in retro and PBR as well; the three channels
     * that exist only inside the kernel keep their numbers and are drawn as an
     * explicit "not available in this mode" card elsewhere. `ptdebugview`
     * remains an alias so existing scripts and config files keep working.
     */
    int debugView {0};
    /**
     * A point light's emitter radius as a fraction of its authored influence
     * radius - it is a sphere, so its subtended solid angle is both its
     * falloff and its penumbra.
     *
     * The influence radius cannot be the emitter radius: it is a range, culled
     * at radius + 64 and promoted to a directional sun past 100, so a lamp
     * would be a room-sized ball. KotOR authored no emitter size, hence a
     * fraction. Brightness-neutral by construction, so this grades penumbra
     * width and how hot a surface can get right against a lamp, nothing else.
     * At 0.2 a radius-5 ceiling panel is a 1-unit fixture subtending 11.5
     * degrees at its own influence radius, against the 8 degrees the old
     * fixed cone gave every light at every distance.
     */
    float ptPointEmitterRatio {0.2f};
    /**
     * Roughness a surface is treated as having after the path has scattered -
     * path regularisation, and a deliberate bias.
     *
     * A tight lobe reached through a bounce is a caustic: the GGX peak at
     * roughness 0.2 is near 200, so a glossy-to-glossy path that then samples
     * the sun returns a spike two orders of magnitude above its neighbours,
     * which no sample count a frame can afford will converge. Raising this
     * blurs indirect reflections and removes more of the speckle; lowering it
     * toward the floor below restores the fireflies.
     *
     * 0.8 rather than 0.5: measured on the specular floor that produced the
     * caustic, 0.5 still left speckle and 0.8 cut it by roughly 4x. The cost is
     * duller bounce reflections, which is the trade taken.
     */
    float ptBounceRoughness {0.8f};
    /**
     * The lowest roughness any surface may take, before regularisation.
     *
     * Odyssey has no roughness channel - diffuse alpha stands in - so this is
     * what stops an authored mirror from becoming a perfect one. It is also
     * why a roughness scale of zero does not produce a mirror; lower this to
     * allow one, and expect the speckle above to come with it.
     */
    float ptRoughnessFloor {0.2f};
    /**
     * Ceiling on a single indirect sample's contribution, or 0 to leave it
     * alone. The blunt instrument beside the two dials above: it truncates
     * energy rather than widening a lobe, so it darkens what it fixes.
     */
    float ptIndirectClamp {0.0f};
    /** The sun is not at a physical distance, so it keeps an angle. Degrees. */
    float ptSunAngularSize {1.0f};
    /**
     * Live per-category material overrides - the calibration programme's
     * primary instrument, ImGui-driven. Indexed by scene::ModelUsage (0-7)
     * plus 8 for meshes without a model root. A colorWeight of 1 flat-paints
     * the category, which makes lighting-interaction bugs self-identifying.
     *
     * Not traced-only: they are applied while the shared material records are
     * built, which the PBR raster resolve reads from as well.
     */
    struct CategoryOverride {
        float color[3] {1.0f, 1.0f, 1.0f};
        float colorWeight {0.0f};
        float roughness {-1.0f}; /**< negative: no override */
        float roughnessScale {1.0f};
        float emissionScale {1.0f};
        float envScale {1.0f};
        /**
         * Scales curated metalness rather than overriding it, so curation
         * still decides which surfaces are metal. The reason it exists: Rf0
         * only becomes chromatic where metalness is non-zero, and every KotOR
         * material is dielectric, so the specular demodulation factor is grey
         * everywhere and that path is untestable without a way to force it.
         */
        float metallicScale {1.0f};
    };
    CategoryOverride categoryOverrides[9] {};
    bool ssao {true};
    bool ssr {true};
    /**
     * The occupant of the common anti-aliasing slot.
     *
     * The default is resolved at the command line, where the render mode is
     * also known - see optionsparser.cpp. There is no mode-dependent fallback
     * below that point: whatever this says is what the slot runs.
     */
    AntiAliasing antialiasing {AntiAliasing::Fxaa};
    /**
     * FSR's built-in RCAS sharpening, 0 to skip the pass entirely. Inert
     * unless the slot is running FSR.
     *
     * RCAS exists to claw back the softness of upscaling, and at NativeAA there
     * is no upscaling to compensate for - hence the conservative default. Do
     * not stack a separate sharpen pass on top of it.
     */
    float fsrSharpness {0.0f};
    /** The tone curve of the grade: 0 none, 1 the Gran Turismo curve. On by
        default - the calibration programme is defined in tonemapped terms.
        Owned by the post-process pass, which is the only stage in any mode
        that applies it, and read there only when @ref grade is set. Not the
        display transform itself, which is never optional. */
    int tonemap {1};
    /** Scene-referred exposure ahead of the tone curve; part of the grade. */
    float exposure {1.0f};
    /**
     * Apply the creative grade - the exposure and the tone curve - in the
     * common post-process pass.
     *
     * Not a switch for the pass itself, which always runs and always performs
     * the one display transform: every mode's scene chain stops at linear
     * scene-referred colour, so without the encode the frame would be
     * presented raw. Off is the ungraded diagnostic - unit exposure, no curve,
     * correctly encoded - and it means the same thing in all three modes.
     *
     * Read only outside retro. That mode's colour is the original's, carried
     * through this pipeline as the inverse of its own encode rather than as
     * radiance, and there is nothing there for an exposure stop or a tone
     * curve to grade. See ScenePipeline::postProcessPass.
     *
     * Spelled `post` on the command line as well, for scripts written before
     * the pass stopped being optional.
     */
    bool grade {true};
    /**
     * Unsharp mask over display colour, the last pass of the frame.
     *
     * Off by default. It is a separate stage from FSR's own RCAS, which
     * corrects that upscaler's softness from inside it; running both sharpens
     * one image twice.
     */
    bool sharpen {false};
    /** Strength of that mask; the neighbour weight of its five-tap cross. */
    float sharpenAmount {0.25f};
    /** Overrides the ARE's authored ShadowOpacity when >= 0. The retail data
        authors only two values, 50 and 205, so this is the knob for judging
        how that byte should map to a strength. */
    /** Diagnostic toggle: strip lightmaps from every material record. */
    bool lightmaps {true};
    float shadowOpacity {-1.0f};
    TextureQuality textureQuality {TextureQuality::High};
    int shadowResolution {2048};
    int anisotropicFiltering {2};
    float drawDistance {kDefaultObjectDrawDistance};
};

} // namespace graphics

} // namespace reone
