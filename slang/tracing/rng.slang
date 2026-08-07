/*
 * Copyright (c) 2026 The reone project contributors
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

// Random number generation and low-level directional sampling for the path
// tracer. Pure functions, with one exception: the blue-noise atlas below is a
// table, and a table the sampler reads is not the same thing as a dependency on
// the scene.

import tracing.resources;

public static const uint kPtBlueNoiseSize = 64u;
public static const uint kPtBlueNoiseTiles = 64u;

public static const float kPtPi = 3.14159265359;
public static const float kPtTwoPi = 6.28318530718;

public uint ptHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

public float ptRandom01(inout uint state) {
    state = ptHash(state);
    return float(state & 0x00ffffffu) / 16777216.0;
}

public float3 ptTangentToWorld(float3 normal, float3 local) {
    float3 reference = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 tangent = normalize(cross(reference, normal));
    float3 bitangent = cross(normal, tangent);
    return tangent * local.x + bitangent * local.y + normal * local.z;
}

public float3 ptCosineHemisphere(float3 normal, float2 xi) {
    float radius = sqrt(xi.x);
    float phi = kPtTwoPi * xi.y;
    float3 local = float3(radius * cos(phi), radius * sin(phi), sqrt(1.0 - xi.x));
    return ptTangentToWorld(normal, local);
}

/**
 * Sample the GGX distribution of VISIBLE normals (Heitz, JCGT 2018), given the
 * view direction. Takes perceptual roughness; alpha is squared internally, as
 * in the raster resolve.
 *
 * Not the plain NDF, which this replaces, and the difference is not a
 * refinement. Pairing an NDF sample with a weight of F is only correct for a
 * visible-normal pdf; with the NDF's own pdf the weight owes a further
 * G * VdotH / (NdotV * NdotH), which is about one head-on and diverges at
 * grazing incidence - so a glossy floor seen at a shallow angle scattered
 * occasional enormous samples, which is what the speckle was. Sampling the
 * visible normals instead makes the weight F * G2/G1, bounded by F, and it is
 * the lower-variance sampler besides: it never proposes a microfacet the
 * viewer cannot see.
 */
public float3 ptGgxVisibleHalfVector(float3 normal, float3 view, float roughness, float2 xi) {
    float a = roughness * roughness;
    // Into the tangent frame the sampler is defined in, then into the
    // hemisphere configuration where the ellipsoid is a sphere.
    float3 reference = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(reference, normal));
    float3 bitangent = cross(normal, tangent);
    float3 viewLocal = float3(dot(view, tangent), dot(view, bitangent), dot(view, normal));
    float3 viewHemi = normalize(float3(a * viewLocal.x, a * viewLocal.y, max(0.0, viewLocal.z)));
    // An orthonormal basis around it, degenerate-safe at normal incidence.
    float lensq = viewHemi.x * viewHemi.x + viewHemi.y * viewHemi.y;
    float3 t1 = lensq > 0.0 ? float3(-viewHemi.y, viewHemi.x, 0.0) * rsqrt(lensq)
                            : float3(1.0, 0.0, 0.0);
    float3 t2 = cross(viewHemi, t1);
    // A disk sample, squashed to the projected area the viewer actually sees.
    float r = sqrt(xi.x);
    float phi = kPtTwoPi * xi.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + viewHemi.z);
    p2 = lerp(sqrt(max(0.0, 1.0 - p1 * p1)), p2, s);
    float3 halfHemi = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * viewHemi;
    float3 halfLocal = normalize(float3(a * halfHemi.x, a * halfHemi.y, max(0.0, halfHemi.z)));
    return tangent * halfLocal.x + bitangent * halfLocal.y + normal * halfLocal.z;
}

/**
 * Interleaved gradient noise (Jimenez, "Next Generation Post Processing in Call
 * of Duty: Advanced Warfare"). A per-pixel dither value whose error is arranged
 * into a fine screen-space pattern rather than scattered white noise, which is
 * what makes it resolve under a temporal filter instead of fighting one.
 */
public float ptInterleavedGradientNoise(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

/**
 * Interleaved gradient noise along a second, different projection.
 *
 * Not the same function with a shifted argument. IGN is a scalar built from one
 * projection of the pixel, dot(p, k); shifting p by a constant only adds a
 * constant to that projection, so the shifted field is the original plus an
 * offset and the two are locked together. A pair built that way lands on a
 * single diagonal of the unit square and never samples off it. Rotating the
 * projection instead gives a genuinely independent field.
 */
float ptInterleavedGradientNoise2(float2 pixel) {
    return frac(52.9829189 * frac(dot(pixel, float2(0.00583715, 0.06711056))));
}

/**
 * A dithered 2D sample for the first shadow ray, decorrelated over time.
 *
 * The temporal offset is applied to the coordinate the noise is evaluated at,
 * not added to its result. That distinction is the whole of it: adding a
 * per-draw constant to the value shifts every pixel on screen by the same
 * amount, so the dither field does not change between draws, it translates
 * rigidly - and along a penumbra, where the threshold between lit and shadowed
 * falls somewhere inside that field, the pattern is seen to scroll. Offsetting
 * the coordinate resamples a different field each draw, which is what the
 * technique is for.
 *
 * What this does not fix, and cannot: IGN is a diagonally striped field, and
 * every animation of it moves all pixels by very nearly the same amount per
 * draw - measured over a 256x256 tile, the spread of the per-pixel step is
 * 0.008 against a step of order 0.5. A near-uniform shift of a striped field is
 * seen as the stripes marching, which is what animated IGN looks like on a
 * penumbra. Breaking the lockstep needs a per-pixel, per-draw offset, and
 * adding one turns the spatial distribution white: high-frequency energy over
 * the same tile falls from 0.967 to 0.805, which is white noise's own value.
 * The two properties are not simultaneously available from one scalar field.
 * A spatiotemporal blue-noise texture is what provides both, and is the
 * intended replacement here.
 */
public float2 ptShadowSample(uint2 pixel, uint sequence) {
    // The counter picks the tile, and a hashed toroidal offset picks where the
    // tile is anchored.
    //
    // The tiles alone are not enough. Sixty-four of them consumed a sample at a
    // time is a period of 64/spp frames - at 2 spp and 300 fps that is 32
    // frames, about a tenth of a second, and a noise field that repeats ten
    // times a second is a field that pulses rather than averages. The offset
    // removes the period: these tiles are built to wrap, so shifting one
    // toroidally yields another valid blue-noise field rather than a damaged
    // one, and a hash of the counter gives a different anchor every draw.
    //
    // Shifting the anchor is not the same mistake as shifting the value. A
    // value offset moves every pixel by one amount and slides the pattern
    // rigidly; this re-anchors the whole field somewhere unrelated each draw,
    // so consecutive draws are independent rather than adjacent.
    uint tile = sequence % kPtBlueNoiseTiles;
    uint2 origin = uint2(tile % 8u, tile / 8u) * kPtBlueNoiseSize;
    uint2 anchor = uint2(ptHash(sequence), ptHash(sequence ^ 0x9e3779b9u)) % kPtBlueNoiseSize;
    uint2 texel = origin + ((pixel + anchor) % kPtBlueNoiseSize);
    // R and G are decorrelated within a tile (measured 0.022), so they serve as
    // the two dimensions directly.
    return blueNoise.Load(int3(int2(texel), 0)).rg;
}

/** The interleaved-gradient fallback, kept for comparison against the texture. */
public float2 ptShadowSampleIgn(uint2 pixel, uint sequence) {
    float2 p = float2(pixel) + float2(47.0, 17.0) * 0.695 * float(sequence % 64u);
    return float2(ptInterleavedGradientNoise(p),
                  ptInterleavedGradientNoise2(p + float2(23.0, 71.0)));
}

// Uniform direction inside a cone: light sources are never points, so every
// shadow ray samples the source's angular extent and shadows get penumbra.
public float3 ptConeSample(float3 direction, float angularRadius, float2 xi) {
    float cosTheta = lerp(1.0, cos(angularRadius), xi.x);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = kPtTwoPi * xi.y;
    return ptTangentToWorld(direction, float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}
