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

// Denoises the direct-diffuse channel, and only that channel, with a radius it
// derives rather than one it is given.
//
// The premise is that a shadow's softness is not a free parameter. A source
// subtending half-angle t, seen from the shaded point, blurs an edge over about
// d*tan(t) of world space at a blocker distance d - the tracer computes that
// per sample and hands it over in alpha. Divide by how much world space one
// pixel covers at this depth and the result is a radius in pixels: exactly as
// wide as the penumbra genuinely is, and zero at a contact edge, where a
// general-purpose denoiser has no way to know it should stop.
//
// This is why it is not NRD's job. A spatial denoiser sized to settle bounce
// noise is sized wrong for a shadow everywhere except by accident; sized from
// the geometry, the filter cannot soften an edge that is not soft.
//
// It blurs demodulated radiance - no albedo, no texture - so nothing here can
// smear surface detail. Only the shadow signal moves.

[[vk::binding(0, 1)]] RWTexture2D<float4> outFiltered;
[[vk::binding(1, 1)]] RWTexture2D<float4> inDirectDiffuse;
[[vk::binding(2, 1)]] RWTexture2D<float> inViewZ;
[[vk::binding(3, 1)]] RWTexture2D<float4> inNormalRoughness;

/** Mirrors ShadowFilterPushConstants in tracingpipeline.cpp. */
struct ShadowFilterPushConstants {
    /**
     * World units covered by one pixel at unit view depth: 2*tan(fovY/2)/height.
     * Multiplied by viewZ it gives the world size of this pixel, which is what
     * turns a world-space penumbra into a pixel radius.
     */
    float pixelWorldPerDepth;
    /** Ceiling on the radius, in pixels, whatever the geometry asks for. */
    float maxRadius;
    /** Rejection: relative view-depth difference a tap may have. */
    float depthTolerance;
    /** Rejection: minimum normal agreement a tap may have. */
    float normalTolerance;
    /** Multiplier on the radius the geometry implies; 1 is the physical answer. */
    float radiusScale;
    /** Floor on that radius, applied only where something blocked the light. */
    float minRadius;
};
[[vk::push_constant]] ShadowFilterPushConstants pushConstants;

static const float kInvalidDepth = 1.0e5;

/**
 * Twelve points on a spiral, near-uniform in area.
 *
 * A disk rather than a separable pass because the radius is per pixel: a
 * separable blur assumes every column shares the row's radius, and across a
 * shadow edge - which is precisely where the radius changes - they do not.
 */
static const float2 kTaps[12] = {
    float2(0.1650, 0.1487), float2(-0.3266, 0.1207), float2(0.1839, -0.4276),
    float2(0.2202, 0.5399), float2(-0.6376, -0.1451), float2(0.6199, -0.3157),
    float2(-0.1666, 0.7273), float2(-0.4262, -0.6675), float2(0.8158, 0.2371),
    float2(-0.7736, 0.4788), float2(0.2564, -0.8853), float2(-0.0855, -0.9469)};

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outFiltered.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;
    int2 pixel = int2(dispatchId.xy);

    float4 center = inDirectDiffuse[pixel];
    float viewZ = abs(inViewZ[pixel]);
    if (viewZ >= kInvalidDepth) {
        outFiltered[pixel] = center;
        return;
    }

    // The median of the 3x3 neighbourhood, not this pixel's own value. One
    // sample per pixel per frame means the penumbra estimate is as noisy as
    // everything else here, and a single pixel that happened to find a distant
    // blocker would otherwise open the filter wide on its own. A median throws
    // that pixel away; a mean would let it vote.
    float window[9];
    int count = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int2 p = clamp(pixel + int2(dx, dy), int2(0, 0), int2(width - 1, height - 1));
            window[count++] = inDirectDiffuse[p].a;
        }
    }
    for (int i = 1; i < 9; ++i) {
        float v = window[i];
        int j = i - 1;
        while (j >= 0 && window[j] > v) {
            window[j + 1] = window[j];
            --j;
        }
        window[j + 1] = v;
    }
    float penumbra = window[4];

    float pixelWorld = max(viewZ * pushConstants.pixelWorldPerDepth, 1e-6);
    float radius = penumbra / pixelWorld * pushConstants.radiusScale;
    // The floor is conditioned on there being a penumbra at all. Applied
    // unconditionally it would blur the unshadowed side of every contact edge,
    // which is the one thing this filter exists to avoid.
    if (penumbra > 0.0) {
        radius = max(radius, pushConstants.minRadius);
    }
    radius = min(radius, pushConstants.maxRadius);
    if (radius < 0.5) {
        // Sharper than a pixel: there is nothing here a filter can improve, and
        // a contact edge is exactly this case.
        outFiltered[pixel] = center;
        return;
    }

    float3 normal = inNormalRoughness[pixel].xyz;
    float3 sum = center.rgb;
    float weight = 1.0;
    for (int t = 0; t < 12; ++t) {
        int2 p = pixel + int2(kTaps[t] * radius);
        if (any(p < int2(0, 0)) || any(p >= int2(width, height))) continue;
        float tapZ = abs(inViewZ[p]);
        if (tapZ >= kInvalidDepth) continue;
        // Guarded on the surface, not on the signal: two points on one plane
        // under one light belong in the same average however differently the
        // shadow fell on them, and that difference is the noise being removed.
        if (abs(tapZ - viewZ) > pushConstants.depthTolerance * viewZ) continue;
        if (dot(inNormalRoughness[p].xyz, normal) < pushConstants.normalTolerance) continue;
        sum += inDirectDiffuse[p].rgb;
        weight += 1.0;
    }
    outFiltered[pixel] = float4(sum / weight, penumbra);
}
