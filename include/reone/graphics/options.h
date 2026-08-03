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
    bool pbr {true};
    /** "raster" normally follows pbr; "path-tracing" selects Vulkan ray queries. */
    std::string mode {"raster"};
    /**
     * Hash the GpuScene upload every frame so --dumptargets can log it. The
     * hash walks every uploaded byte, which is measurable CPU per frame, so
     * it is on only when a dump was requested - the one consumer it has.
     */
    bool hashUploads {false};

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
     * Light-balance knobs for the traced mode. They scale sources, not the
     * image: sky is authored background geometry (semantic, never
     * color-based), emissive is every other emitter, and lightmap scales
     * the baked radiance cache that stands in for light sources the tracer
     * cannot see yet.
     */
    /** Calibration session of 2026-07-29: sky and emissive at 2.5, the
        lightmap cache retired to zero - two bounces of real transport
        replace it - and the sun at 2.5 where the Dantooine dusk reads as a
        sun. All still dials; these are the graded defaults. */
    float ptSkyIntensity {2.5f};
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
     * Anti-alias with FidelityFX Super Resolution at NativeAA.
     *
     * This is the only temporal resolve left. The composite used to carry a
     * hand-rolled TAA behind a blend dial; FSR measured 4.6x better on edges,
     * so that one is gone rather than kept as a switchable alternative. With
     * FSR on the composite stops at linear HDR for it to resolve and
     * pt_tonemap to finish; with it off, or in a build without FSR, the
     * composite tonemaps directly and the frame has no anti-aliasing.
     */
    bool ptFsr {true};
    /**
     * FSR's built-in RCAS sharpening, 0 to skip the pass entirely.
     *
     * RCAS exists to claw back the softness of upscaling, and at NativeAA there
     * is no upscaling to compensate for - hence the conservative default. Do
     * not stack a separate sharpen pass on top of it.
     */
    float ptFsrSharpness {0.0f};
    /** Debug view: 0 off, then the values in tracing/debug.slang. */
    int ptDebugView {0};
    /** Display transform: 0 off, 1 ACES. On by default - the calibration
        programme is defined in tonemapped terms. */
    int ptTonemap {1};
    /** Scene-referred exposure ahead of the tonemap. */
    float ptExposure {1.0f};
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
    /** The sun is not at a physical distance, so it keeps an angle. Degrees. */
    float ptSunAngularSize {1.0f};
    /**
     * Live per-category material overrides for the traced image - the
     * calibration programme's primary instrument, ImGui-driven. Indexed by
     * scene::ModelUsage (0-7) plus 8 for meshes without a model root. A
     * colorWeight of 1 flat-paints the category, which makes
     * lighting-interaction bugs self-identifying.
     */
    struct PtCategoryOverride {
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
    PtCategoryOverride ptCategoryOverrides[9] {};
    bool ssao {true};
    bool ssr {true};
    bool fxaa {true};
    bool sharpen {true};
    /**
     * Offset the projection by a sub-pixel jitter each frame. Motion vectors are
     * produced regardless; this only controls the jitter itself, and is off by
     * default because nothing resolves it yet.
     */
    bool taaJitter {false};
    TextureQuality textureQuality {TextureQuality::High};
    int shadowResolution {2048};
    int anisotropicFiltering {2};
    float drawDistance {kDefaultObjectDrawDistance};
};

} // namespace graphics

} // namespace reone
