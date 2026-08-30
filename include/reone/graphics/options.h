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

#include <algorithm>

#include "types.h"
#include "grasscard.h"

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
    /**
     * DLSS Ray Reconstruction: one network replacing both the denoiser and the
     * upscaler. Available only where the user has supplied Streamline's DLLs
     * and the adapter is an RTX part - the pipeline falls back to Fsr and says
     * so once when it is not, so selecting it is never an error.
     */
    DlssRr,
};

/**
 * DLSS-RR's quality modes, in Streamline's own order so the index is the value.
 *
 * Each names a render-scale ratio; kDlssModeScale beside it is the mapping
 * NVIDIA publishes, reproduced rather than invented.
 */
enum class DlssMode {
    Dlaa,
    Quality,
    Balanced,
    Performance,
    UltraPerformance,
};

inline float dlssModeScale(DlssMode mode) {
    switch (mode) {
    case DlssMode::Quality: return 1.0f / 1.5f;
    case DlssMode::Balanced: return 1.0f / 1.7f;
    case DlssMode::Performance: return 1.0f / 2.0f;
    case DlssMode::UltraPerformance: return 1.0f / 3.0f;
    default: return 1.0f;
    }
}

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
/**
 * The bounce range, defined once.
 *
 * The command line, the registry, the settings slider and the tracer's push
 * constants all bound this value, and four copies of a literal pair is four
 * places to miss when the range changes - which is how it stayed 1-based after
 * the shader was ready for zero.
 */
constexpr int kMinPtBounces = 0;
constexpr int kMaxPtBounces = 8;
/** Light samples per shading vertex, defined once for the same reason. */
constexpr int kMinPtNeeSamples = 1;
constexpr int kMaxPtNeeSamples = 8;

/**
 * Upper bound on GraphicsOptions::grassTriangleBudget.
 *
 * Eight million rather than a quarter of one: the budget has to be able to
 * express what the density dials can ask for, and a module's authored clusters
 * times a handful of blades each runs to millions. A ceiling below that turns
 * every other grass dial into a no-op, which is what it did.
 */
constexpr int kMaxGrassTriangleBudget = 1048576;

/**
 * What settles the primary-vertex direct channel.
 *
 * Two answers, not a strength dial: leave it to the temporal resolve, or give
 * it a denoiser that measures whether a pixel needs filtering at all.
 *
 * A third once sat between them - a blur sized from the penumbra the geometry
 * implied. It was removed rather than defaulted off: measured over 32 FSR
 * frames on the region it touched, every filtered variant was LESS temporally
 * stable than none (0.7584 at the physical radius, 0.7507 forced to 8 pixels,
 * against 0.7429 unfiltered) and none was quieter. Blue noise puts its error
 * at high spatial frequency, which is what a temporal resolve averages away;
 * a spatial blur moves that error down into low frequency, where a clamp
 * cannot tell it from signal, so it survives and then boils.
 */
enum class ShadowFilter {
    Off,
    /** A second NRD denoiser, fed this channel alone. */
    Denoiser
};

/** Channels the post-denoise resolve produces, rather than the trace kernel. */
constexpr bool isResolveDebugView(int view) {
    return view >= 17 && view <= 19;
}

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

/**
 * Retro's caster budget, from the original's own videoquality.2da:
 * NumShadowCastingLights is 1 at fast, low and good and 3 at best. Retro takes
 * the best-quality figure, because a mode that exists to reproduce the original
 * should reproduce the setting a player would have chosen rather than the one a
 * 2003 machine forced.
 */
constexpr int kRetroShadowCasters = 3;

/**
 * How many shadow casters a mode may hold, by kind.
 *
 * One declaration because it has two consumers that must agree: the scene
 * selects against it, and the pipeline sizes its shadow images from it. Two
 * copies of this rule would show up as a caster with no map behind it.
 */
struct ShadowBudget {
    int directional {0};
    int point {0};
    int total {0};
};

struct GraphicsOptions {
    int width {1024};
    int height {768};
    int winScale {100};
    bool fullscreen {false};
    /** Hide the presentation window while retaining a renderable surface. */
    bool headless {false};
    /** Draw 3D scene content: the world and scene-backed GUI panels. */
    bool sceneRender {true};
    bool vsync {true};
    /**
     * How much light a thin surface passes to its far side.
     *
     * Leaves, cloth and grass are cutouts standing in for things with no
     * thickness, and a solid-surface model sends the half of one facing away
     * from the sun to black. They are lit from both sides instead, the far side
     * dimmed by this rather than matching the near side.
     *
     * A flat scalar, and graded rather than derived: there is no thickness, no
     * angle dependence and no forward lobe behind it, so it says how much
     * light comes through and nothing about how. Real foliage transmits far
     * more at grazing angles and scatters it toward the light, which is what
     * makes a backlit leaf glow at its silhouette - none of that is modelled,
     * so the number is a look rather than a measurement.
     */
    float thinTransmission {0.8f};
    /**
     * How far the path tracer bends a ray passing through a transparent
     * surface, 0 for not at all.
     *
     * Deliberately separate from thinTransmission, which answers a different
     * question - how much light a thin surface passes to its far side, for
     * shading - and would move two behaviours from one slider if it were
     * reused here.
     *
     * Invention, and priced as such. Odyssey glass is a flat card with no
     * thickness and no authored index of refraction, so any bend at all is
     * something the original did not have; the default is therefore off, and
     * the frame at 0 is the frame with the feature absent.
     */
    float ptRefraction {0.0f};
    /**
     * Exponent authored surface colour is decoded with, in PBR and in path
     * tracing alike.
     *
     * Not a path-tracing setting: the two modes are meant to differ only in
     * how light reaches a surface - analytical lights with shadow maps and
     * lightmaps on one side, traced transport on the other - and never in what
     * the surface is. A decode that applied to one of them would be a material
     * difference, which is the thing that must not exist.
     *
     * 2.2 is the sRGB-correct value. It is not what Odyssey content was
     * authored for: xoreos, KotOR.js and kvp all multiply the texel by light
     * unconverted, as the original did, so decoding squares an albedo the
     * artist picked directly - 0.5 becomes 0.22 - and a path tracer compounds
     * that per bounce. 1.0 reproduces the reference engines exactly.
     * Reflectance only; light colour, emission and the output encode are not
     * affected, the last because post-process inverts a fixed 2.2.
     */
    float albedoGamma {2.2f};
    /**
     * Lights a frame may carry, out of the kMaxLights the uniform block is
     * sized for.
     *
     * Below the ceiling on purpose. The light loop runs per pixel in the raster
     * resolves and the selection table is walked per path vertex in the tracer,
     * so the cost is in how many are admitted rather than in how many could be;
     * the ceiling only costs uniform bytes.
     */
    int maxLights {48};
    /**
     * Shadow-casting light budgets, by kind, for the corrected modes.
     *
     * Two numbers rather than one because the kinds cost differently: a
     * directional caster is four cascade layers at shadowResolution, a point
     * caster is a whole cube at pointShadowResolution. Retro does not use these
     * - it takes one combined budget, the original's own
     * NumShadowCastingLights; see SceneGraph::shadowBudget.
     *
     * These are ceilings on what a frame MAY hold, and they cost memory
     * whether or not they fill. What fills them is the authored per-light
     * shadow flag narrowed by each light's own radius, and that is a much
     * tighter constraint: the largest set measured across ten K1 modules was
     * four (RECORD 3.13). Only occupied slots are rendered, so raising these
     * buys headroom for dense scenes rather than per-frame cost.
     */
    int maxDirectionalShadows {2};
    int maxPointShadows {16};
    /**
     * Cube face resolution for point shadow maps.
     *
     * Separate from shadowResolution, and much smaller, because the two cover
     * incomparable areas: a directional cascade spans the visible world, while
     * a point map covers only the light's own radius - a lamp lighting a few
     * metres. At 512 a face carries roughly the texel density the 2048
     * directional map gives the scene, and the whole 16-slot ceiling costs
     * about 100 MB of D32 against 400 MB at 1024.
     */
    int pointShadowResolution {512};
    bool grass {true};
    /**
     * Draw the shadow of the selected shadow light, or none at all.
     *
     * Off means the scene has no shadow light for the frame: no shadow pass
     * runs and no shadow term reaches the uniforms, rather than a pass that
     * renders and resolves to nothing. It exists so a comparison against
     * another build can exclude a subsystem whose two implementations are
     * known to differ, and it must therefore mean the same thing in both
     * builds - a switch that disables slightly different work on each side
     * measures itself.
     *
     * Retro only in effect: path tracing's shadows come from the shadow ray,
     * which this does not reach.
     */
    bool shadows {true};
    /**
     * The global fog master switch, honoured by every mode.
     *
     * Fog stays per-surface beneath it - a material without the fog feature is
     * never fogged, and the near/far distances are the scene's - but off
     * suppresses it everywhere: the traced kernel writes a zero blend amount and
     * the composite skips the blend, so the two modes turn it off together
     * rather than one keeping a haze the other dropped.
     */
    bool fog {true};
    /**
     * Height of the fog gradient, in world units: the altitude above the
     * walkmesh at which density has fallen to a hundredth of its ground value.
     *
     * Exponential in height rather than a distance ramp, which would call the
     * far plane fully fogged and swallow the sky. A shallow gradient is a
     * ground layer and leaves the sky clear - so the sky can read less fogged
     * than a ridge in front of it - while tens of units becomes atmospheric
     * haze. On danm14ab the two cross over near 64.
     *
     * Density is calibrated to the area's authored ramp, not dialled.
     */
    float fogHeight {8.0f};
    /** Admit emitter particles, or leave them out of the frame entirely. */
    bool particles {true};
    /**
     * Draw the forward transparency pass, or stop at the opaque image.
     *
     * Everything non-opaque goes through one pass - blended and additive
     * geometry, sabers, emitter particles, the transparent halves of models -
     * so switching it off leaves exactly the opaque frame the resolve produced.
     * That is what makes it a diagnostic: an artefact that survives is not
     * transparency's, and one that vanishes is.
     *
     * Distinct from `particles`, which decides whether emitters are admitted to
     * the scene at all. This draws or skips the pass they would have been drawn
     * in, along with everything else in it.
     */
    bool transparency {true};
    /**
     * Draw the halo billboards authored on flare-bearing lights.
     *
     * Off by default. The billboard path exists but has never been reachable:
     * RenderCategory::LensFlare is absent from every admission filter, so the
     * flares a light authors are registered, updated, and then dropped. Turning
     * this on admits them in every render mode. It is a dial rather than a
     * straight fix because it adds something to PBR and path-tracing frames
     * that has never been in them.
     */
    bool lensFlares {false};
    /**
     * Blur what self-illuminated surfaces put above the threshold back over the
     * frame, in every render mode.
     *
     * A common stage rather than a retro-local one: the reference build derives
     * its bloom from a second opaque attachment, but the quantity it extracts -
     * emissive surfaces at the top of the range - is available to every mode
     * from the G-buffer, so one pass serves all three.
     */
    bool bloom {true};
    /** Display-space level a lit texel must pass before it blooms. */
    float bloomThreshold {0.95f};
    /** Scale on what the blur adds back. */
    float bloomIntensity {1.0f};
    /** Multiplier on the area's authored Grass_Density, so areas keep their variation. */
    float grassDensity {8.0f};
    GrassCardShape grassCardShape {GrassCardShape::Quad};
    /** Sides of a fitted k-gon, 3..16. */
    int grassCardSides {5};
    /** Cells across a fitted grid, 2..32. */
    int grassCardGrid {8};
    /** Card width over length; grassWidth is the corresponding strand dial. */
    float grassCardAspect {1.0f};
    /**
     * Grass defaults to the legacy primitive for its render mode: cards under
     * Retro and strands elsewhere. A strand is a strip of GrassSegments quad
     * segments closed by one triangle at the tip, generated on the GPU from a
     * hash of the blade's identity so the field is bit-identical frame to frame.
     *
     * Lengths and widths are multiples of the area's authored quad size rather
     * than world units, so an area that authored small grass keeps it.
     */
    float grassRadius {25.0f};
    /**
     * Which way a blade faces, and how far from that it may stray, in radians.
     *
     * The variation is a full turn by default, which is the random scatter a
     * field wants. At zero every blade in the area points the same way, which
     * is what a wind direction or a mown lawn looks like; between the two it
     * leans without marching in step.
     */
    float grassOrientation {0.0f};
    float grassOrientationVariance {6.28318531f};
    /**
     * Wind. Strength is the extra bend at full gust, in radians, and zero is
     * still air.
     *
     * The model is two travelling waves rather than noise: a fast one that
     * carries the rustle and a slow, longer one that carries the gust, summed
     * and phase-shifted per blade so neighbours do not move in lockstep. Both
     * bend the blade downwind by rotating its arc toward the wind direction,
     * so a blade already leaning that way straightens and one leaning against
     * it folds over - which is what makes a field look like it is being
     * crossed rather than shaken.
     */
    float grassWindStrength {0.35f};
    /** Where it blows from, radians, in the same frame as the blade orientation. */
    float grassWindDirection {0.0f};
    /** How fast the rustle travels, and how far apart its crests are in world units. */
    float grassWindSpeed {1.4f};
    float grassWindWavelength {6.0f};
    /** Share of the strength carried by the slow gust rather than the rustle. */
    float grassWindGust {0.6f};
    /** Total bend of the arc, radians, and how much it varies between blades. */
    float grassCurvature {0.45f};
    float grassCurvatureVariance {0.25f};
    /** Fraction of slots left empty. Buys clumping; density sets the grid. */
    float grassSparsity {0.0f};
    /** Displacement from the slot centre, in cells. Above 1 blades cross into neighbours. */
    float grassDisplacement {0.03f};
    /**
     * The blade albedo outright, not a tint: strands carry no texture, so
     * nothing else contributes. White here is white grass.
     */
    glm::vec3 grassColor {0.30f, 0.32f, 0.11f};
    /**
     * Blade roughness, set outright rather than derived.
     *
     * Roughness normally comes from the diffuse texture's alpha, and a strand
     * has no texture - so without this every blade would take the fully-rough
     * default and the field would have no sheen at all.
     */
    float grassRoughness {0.8f};
    /**
     * Blades grown from each authored cluster.
     *
     * The area's records were authored against cardboard, where one cluster is
     * a card whose texture already depicts a tuft. Drawing one strand where a
     * card used to stand therefore replaces a tuft with a single blade, which
     * is why the same cluster count that looked like a field looks like
     * stubble. Each cluster can now grow a small handful, hashed apart from one
     * another inside the cell.
     *
     * Defaults to one, because while the triangle budget binds this only
     * redistributes: the budget divides by blades-per-cluster, so raising it
     * thins the surviving clusters by the same factor and gathers the same
     * total into tufts. Measured green coverage fell from 0.017 to 0.010 per
     * cent going from one to eight - but only because the ceiling was binding
     * and divides by this. Raise the triangle budget alongside it and this
     * multiplies density as intended.
     */
    int grassBladesPerCluster {1};
    /**
     * Blade height as a multiple of the area's authored quad size.
     *
     * One, with the offset below, reproduces the extent the reference builds:
     * a quad one quad size tall whose centre is raised by nine tenths of its
     * half height, spanning the surface it stands on from a twentieth below to
     * nineteen twentieths above.
     *
     * The reach matters more than it looks. Grass is scattered over the room
     * model's AABB node, which is the walkmesh, while the ground drawn is a
     * separate and finer mesh, so a root lands wherever the two disagree.
     * Measured on Dantooine's estate the meadow disagrees by 0.03 world units
     * but its planter beds are drawn as mounds up to 0.78 above the flat
     * walkmesh face grass is placed on - so a blade that stops short is a
     * planter with no grass in it, not a blade slightly too short.
     */
    float grassLength {1.0f};
    float grassLengthVariance {0.3f};
    /** Blade width as a fraction of its length. */
    float grassWidth {0.1f};
    /** Sinks the root under the ground, as a fraction of length, so it does not float. */
    float grassYOffset {-0.05f};
    /**
     * Ceiling on grass triangles in the scene, at nine per blade.
     *
     * A hard cap, not an allocator: blades lie across the whole module at the
     * authored cluster density and this scales that set down uniformly. It does
     * not concentrate anything near the camera - blades beyond the draw radius
     * still hold their share - so a dense near field means raising this until
     * the near field looks right and accepting that most of it is spent out of
     * sight. Tightening that is a compaction pass, deliberately not built yet.
     *
     * Hence the headroom: 41k authored clusters at eight blades each is three
     * million triangles, and the ceiling has to be able to say yes to that
     * before any of the density dials mean anything. Zero disables the cap.
     */
    int grassTriangleBudget {524288};
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
        sun. Each has a pt* and a pbr* dial: the modes are meant to differ in
        how light REACHES a surface, but their grades are authored against
        different transport, and one shared number made every adjustment a
        question of which mode happened to be running. The traced defaults are
        the 2026-07-29 grade (2.5/2.5/0.0); the raster ones keep authored
        levels with the bake as their indirect light (1.0/1.0/1.0). */
    /**
     * Strength of the baked lightmap, per mode.
     *
     * The bake IS the indirect light in PBR, while the tracer computes that
     * transport for real and retires the bake toward zero so it is not counted
     * twice. Two dials rather than one with per-mode defaults: grading one mode
     * is not meant to move the other, and a single dial made every adjustment a
     * question of which mode happened to be running.
     */
    float ptLightmapIntensity {0.0f};
    float pbrLightmapIntensity {1.0f};
    float ptDirectIntensity {1.0f};
    float pbrDirectIntensity {1.0f};
    float ptSunIntensity {2.5f};
    float pbrSunIntensity {2.5f};
    /**
     * Path depth after the primary hit.
     *
     * Zero is a real setting, not a floor to be clamped away: the primary
     * vertex still draws next-event estimation, so the frame is direct lighting
     * with no indirect at all. That is the reference the indirect terms are
     * judged against, and it is what the raster modes approximate.
     */
    int ptBounces {2};
    /**
     * Next-event estimation: draw a light at each shading vertex and trace a
     * shadow ray at it.
     *
     * On, always, outside a diagnostic. Off, a path finds light only where its
     * BSDF ray happens to land, and KotOR's lights are analytic rather than
     * geometry - so the direct term goes to nothing and what remains is emissive
     * surfaces and sky. That is what makes it worth having: it separates what
     * NEE contributes from what the BSDF ray finds on its own.
     */
    bool ptNee {true};
    /**
     * Light samples per shading vertex.
     *
     * The variance in direct light falls as 1/sqrt of this, for a linear cost
     * in shadow rays. It is cheaper than the equivalent in samples per pixel,
     * which re-traces the primary hit as well - so a scene whose noise is
     * mostly shadow noise is better served here.
     */
    int ptNeeSamples {1};
    /**
     * Scatter rays see grass.
     *
     * On by default: with the instance mask the traversal costs nothing
     * measurable, and skipping it lets bounce rays see sky through the canopy,
     * which brightened the ground under grass by a quarter.
     */
    bool ptGrassScatter {true};
    /**
     * Shadow rays see grass. Traced once per light per shading vertex and
     * reused across next-event draws, so more draws cost no more grass
     * traversals.
     */
    bool ptGrassShadows {true};
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
     * Parity mode: both renderers output ONLY albedo * unoccluded direct
     * diffuse, summed over every light, through one shared function
     * (lib/direct_light.slang). Exists to prove the two final images are the
     * same arithmetic before their sanctioned differences - occlusion method,
     * specular source, GI source - are layered back on.
     */
    bool parityDirect {false};
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
     * What settles the primary-vertex direct channel.
     *
     * Off leaves it to the temporal resolve, which is the default and is
     * measured to be the better of the two on a signal carrying blue noise.
     * Denoiser gives the channel its own NRD instance, which estimates
     * variance per pixel and sizes its kernel from that, so a converged region
     * keeps its detail - see TracingDenoiserInputs::directRadianceHitDist for
     * why it cannot simply be summed into the diffuse channel and denoised
     * there.
     */
    ShadowFilter ptShadowFilter {ShadowFilter::Off};
    /**
     * Which NRD denoiser runs. REBLUR is the cheaper, blurrier one and was the
     * only choice here; RELAX keeps edges and specular detail at a higher cost,
     * which is the trade this content wants. Staged, not live: NRD fixes the
     * denoiser when the instance is built.
     */
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
    int ptNrdHistoryFixFrames {3};
    /**
     * The direct denoiser's own history and kernel, separate from the bounce
     * channel's above.
     *
     * Short by comparison, and deliberately: a shadow edge moves with whatever
     * casts it, so history that suits slow indirect light reads as lag here.
     * Few A-trous passes and a tight luminance phi for the same reason the
     * prepass is forced off for this denoiser - the signal arrives converged
     * outside the penumbra, and width spent there is spent on detail.
     */
    float ptNrdDirectAccumulationTime {0.15f};
    int ptNrdDirectAtrousIterations {3};
    float ptNrdDirectPhiLuminance {1.0f};
    float ptNrdDiffusePrepassBlurRadius {30.0f};
    float ptNrdSpecularPrepassBlurRadius {50.0f};
    float ptNrdLobeAngleFraction {0.15f};
    float ptNrdRoughnessFraction {0.15f};
    float ptNrdDisocclusionThreshold {0.01f};
    bool ptNrdAntiFirefly {true};
    /** REBLUR only. */
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
     * A rough surface - no environment map, Odyssey's cue for "not shiny" -
     * takes its PBR properties from the category outright: absolute roughness,
     * metalness and F0. Per-object curation still applies on top.
     */
    struct RoughOverride {
        float color[3] {1.0f, 1.0f, 1.0f};
        float colorWeight {0.0f};
        float roughness {1.0f};
        float metallic {0.0f};
        float f0 {0.05f};
    };
    /**
     * A reflective surface keeps the texel's alpha as its roughness stand-in
     * and the authored mirror share as its reflectance lift; the category
     * grades those (a negative roughness leaves the texel alone) and sets F0.
     */
    struct ReflectiveOverride {
        float color[3] {1.0f, 1.0f, 1.0f};
        float colorWeight {0.0f};
        float roughness {-1.0f};
        float roughnessScale {1.0f};
        float metallicScale {1.0f};
        float f0 {0.04f};
    };
    /** Emission grade: off leaves the authored radiance at its encoding. */
    struct EmissionOverride {
        bool enabled {false};
        float intensity {1.0f};
        float gamma {2.2f};
    };
    struct CategoryOverride {
        RoughOverride rough;
        ReflectiveOverride reflective;
        EmissionOverride emission;
    };
    CategoryOverride categoryOverrides[9] {};
    /** The unlit-emissive class: the sky shell and painted backdrops. */
    EmissionOverride skyRoomEmission;
    /** The baked sky cube: intensity, and a re-encode exponent over its 2.2 bake. */
    float skyboxIntensity {1.0f};
    float skyboxGamma {2.2f};
    bool ssao {true};
    bool ssr {true};
    /**
     * The debug overlay: wireframe bounding boxes and name labels for scene
     * objects and lights, drawn over the finished image in every render mode.
     * Lines are depth-tested against the G-buffer per pixel; the occluded part
     * is drawn dimmed rather than dropped. A diagnostic, off by default.
     */
    bool debugOverlay {false};
    /**
     * The occupant of the common anti-aliasing slot.
     *
     * The default is resolved at the command line, where the render mode is
     * also known - see optionsparser.cpp. There is no mode-dependent fallback
     * below that point: whatever this says is what the slot runs.
     */
    AntiAliasing antialiasing {AntiAliasing::Fxaa};
    /**
     * DLSS-RR's quality mode, which is how DLSS expresses a render scale.
     *
     * It OWNS renderScale while DLSS is the occupant: the mode names a ratio
     * (DLAA 1.00, Quality 0.67, Balanced 0.58, Performance 0.50, Ultra
     * Performance 0.33) and the two would otherwise be free to disagree about
     * the same number. Inert under any other occupant.
     */
    DlssMode dlssMode {DlssMode::Dlaa};
    /**
     * Sharpening, 0 to skip it, wherever the frame is sharpened at all.
     *
     * One dial for three implementations, because only ever one of them runs:
     * FSR takes it as RCAS from inside the upscaler, DLSS-RR as its own
     * internal sharpening, and with no upscaler in the slot it drives the
     * postprocess unsharp mask over display colour. That routing is what makes
     * double-sharpening unrepresentable - it used to be two independent dials
     * with a comment asking you not to raise both.
     *
     * The three do not agree numerically: RCAS, a neural sharpener and a
     * five-tap unsharp mask given the same 0.5 do not produce the same picture,
     * so changing resolver changes apparent sharpness. Conservative default for
     * the same reason RCAS had one - at native resolution there is no upscaling
     * softness to correct.
     */
    float sharpness {0.0f};
    /**
     * Trace and raster at this fraction of the display resolution, with FSR
     * upscaling the result. 1 is NativeAA - the same resolution either side of
     * the slot, which is what this engine ran exclusively until now.
     *
     * Only FSR can honour it: it is the one resolve in the slot that changes
     * resolution, so the scale is forced back to 1 when anything else occupies
     * the slot rather than silently rendering small and stretching. NRD sits
     * upstream of the upscaler and denoises at this resolution, which is where
     * its own guidance puts it.
     *
     * FSR's quality modes are 1/1.5 quality, 1/1.7 balanced, 1/2 performance,
     * 1/3 ultra performance. A free scale rather than an enum because the
     * jitter phase count derives from the ratio anyway.
     */
    float renderScale {1.0f};
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
    float guiScale {1.0f};
    float guiTextScale {0.5f};
    float guiDialogTextScale {0.6f};
    float guiBorderScale {1.0f};
    float guiListScale {0.5f};
};

/**
 * The resolution the scene is traced and rastered at, given what it presents to.
 *
 * One definition because two callers have to agree exactly: the scene pipeline
 * sizes the G-buffer and the tail's first half with it, and the ray-query
 * pipeline sizes the tracer with it. A disagreement is a tracer writing into
 * aux images the resolve reads at a different extent, which is not a crash.
 */
inline glm::ivec2 renderExtentFor(const GraphicsOptions &options, glm::ivec2 displayExtent) {
    // Only an upscaling resolve changes resolution across the anti-aliasing
    // slot, so only those can honour a scale; anything else renders at display
    // resolution rather than small and stretched.
    if (options.antialiasing != AntiAliasing::Fsr &&
        options.antialiasing != AntiAliasing::DlssRr) {
        return displayExtent;
    }
    // Under DLSS the mode owns the ratio - see GraphicsOptions::dlssMode.
    const float scale =
        options.antialiasing == AntiAliasing::DlssRr
            ? dlssModeScale(options.dlssMode)
            : glm::clamp(options.renderScale, 0.25f, 1.0f);
    return glm::max(glm::ivec2(1), glm::ivec2(glm::round(glm::vec2(displayExtent) * scale)));
}

/**
 * Retro's is one number rather than two because that is how the original
 * expressed it - the authored per-light shadow flag decides the kind, and in
 * the shipped content that flag sits on point lights far more often than on the
 * sun. The corrected modes budget per kind instead, because they pay
 * differently for the two: a directional caster is four cascade layers at
 * shadowResolution, a point caster is a whole cube at pointShadowResolution.
 */
inline ShadowBudget shadowBudgetFor(const GraphicsOptions &opts) {
    if (opts.mode == RenderMode::Retro) {
        // One combined figure, because the original expressed it as one and
        // did not distinguish the kinds. `total` binds here and the per-kind
        // entries only stop one kind monopolising it.
        return {kRetroShadowCasters, kRetroShadowCasters, kRetroShadowCasters};
    }
    // Independent, and deliberately so: the two kinds write to different
    // images, so there is no shared resource for a combined cap to protect.
    // Sharing one made a directional caster competable-for - point lights
    // holding slots could starve the sun, whose shadow is the one shadow a
    // scene can least afford to lose. `total` is their sum, so it never binds.
    const int directional = std::max(0, opts.maxDirectionalShadows);
    const int point = std::max(0, opts.maxPointShadows);
    return {directional, point, directional + point};
}

} // namespace graphics

} // namespace reone
