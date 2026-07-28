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
     * Light-balance knobs for the traced mode, all defaulting to neutral.
     * They scale sources, not the image: sky is fully self-illuminated
     * geometry (luma >= 0.99, the engine's own test), emissive is every other
     * emitter, and lightmap scales the baked radiance cache that stands in
     * for light sources the tracer cannot see yet.
     */
    float ptSkyIntensity {1.0f};
    float ptEmissiveIntensity {1.0f};
    float ptLightmapIntensity {1.0f};
    float ptDirectIntensity {1.0f};
    /** Directional sun intensity, independent from the point-light dial.
        Graded against the retro look rather than any parity target; 2.5 is
        where the Dantooine dusk sun reads as a sun. */
    float ptSunIntensity {2.5f};
    /** Path depth after the primary hit. */
    int ptBounces {1};
    /** Secondary-ray origin offset along the geometric normal, world units. */
    float ptRayOffset {0.01f};
    /** GPU trace-stats counters; off by default, the atomics cost frame time. */
    bool ptTraceStats {false};
    /** Debug view: 0 off, then categories, emissive, normals, roughness,
        lightmap, albedo - matches kDebugView* in slang/rayquery.slang. */
    int ptDebugView {0};
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
    /** World ambient scaled by how little sky each pixel's hemisphere saw. */
    float ptWorldAmbient {1.0f};
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
