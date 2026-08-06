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
     * REBLUR tuning, exposed 1:1 in the Path tracing panel. NRD's defaults
     * are graded for 0.5-1 spp production signals and over-commit against
     * this tracer's 4 spp; these are the user-graded values from the
     * 2026-07-29 session: minimal spatial filtering, moderate accumulation,
     * rejection opened wide so foliage accumulates.
     */
    int ptNrdMaxAccumulatedFrames {6};
    int ptNrdMaxFastAccumulatedFrames {1};
    int ptNrdMaxStabilizedFrames {30};
    int ptNrdHistoryFixFrames {4};
    float ptNrdDiffusePrepassBlurRadius {1.0f};
    float ptNrdSpecularPrepassBlurRadius {1.0f};
    float ptNrdMinBlurRadius {0.5f};
    float ptNrdMaxBlurRadius {32.0f};
    float ptNrdLobeAngleFraction {0.77f};
    float ptNrdRoughnessFraction {0.74f};
    float ptNrdPlaneDistanceSensitivity {0.099f};
    float ptNrdDisocclusionThreshold {0.003f};
    bool ptNrdAntiFirefly {true};
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
