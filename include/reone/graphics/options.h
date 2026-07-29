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
    bool pbr {true};
    /** "raster" normally follows pbr; "path-tracing" selects Vulkan ray queries. */
    std::string mode {"raster"};

    // Path tracing

    /**
     * Hemisphere samples per pixel for the second bounce.
     *
     * Cost is very close to linear - measured on a 5090 at 1920x1080 in
     * danm14ab, 1 sample costs 4.9 ms/frame and 16 costs 38.8. Noise falls as
     * the square root, so doubling this halves neither. Eight is the point
     * where the image reads clearly while the frame still moves.
     */
    int pathTracingSamples {4};

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
     * are graded for 0.5-1 spp production signals; at 4 spp they over-blur
     * both spatially and temporally, so the local defaults land softer.
     */
    int ptNrdMaxAccumulatedFrames {12};
    int ptNrdMaxFastAccumulatedFrames {4};
    int ptNrdMaxStabilizedFrames {8};
    int ptNrdHistoryFixFrames {2};
    float ptNrdDiffusePrepassBlurRadius {8.0f};
    float ptNrdSpecularPrepassBlurRadius {16.0f};
    float ptNrdMinBlurRadius {1.0f};
    float ptNrdMaxBlurRadius {10.0f};
    float ptNrdLobeAngleFraction {0.25f};
    float ptNrdRoughnessFraction {0.15f};
    float ptNrdPlaneDistanceSensitivity {0.05f};
    float ptNrdDisocclusionThreshold {0.01f};
    bool ptNrdAntiFirefly {true};
    /** History weight of the composite's noise-free TAA. */
    float ptTaaBlend {0.85f};
    /** Debug view: 0 off, then categories, emissive, normals, roughness,
        lightmap, albedo - matches kDebugView* in slang/rayquery.slang. */
    int ptDebugView {0};
    /** Display transform: 0 off, 1 ACES. On by default - the calibration
        programme is defined in tonemapped terms. */
    int ptTonemap {1};
    /** Scene-referred exposure ahead of the tonemap. */
    float ptExposure {1.0f};
    /** Angular radius of light sources, degrees. Never zero: a light source
        is never a point. Points wide for soft penumbras, the sun sharp. */
    float ptPointAngularSize {8.0f};
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
        float emissionScale {1.0f};
        float envScale {1.0f};
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
