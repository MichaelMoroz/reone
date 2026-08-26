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

// The debug overlay: wireframe bounding boxes over the finished frame.
//
// One draw per box. The eight world-space corners arrive through the aabb
// uniform block - the same per-draw dynamic-offset idiom every local block
// uses - and the vertex stage synthesises the twelve edges from SV_VertexID:
// six vertices per edge, a screen-space quad two half-widths wide, so no
// vertex buffer and no line topology are needed. Drawn over the display image
// in every render mode, after the display transform: the overlay is a
// diagnostic drawn ON the picture, not part of it, so it must not be graded,
// anti-aliased or sharpened.
//
// Occlusion is a per-pixel decision, not a depth test: the fragment compares
// the edge's own depth against the G-buffer depth at the pixel and DIMS the
// occluded part rather than discarding it - a hidden box still tells you where
// the object is, and the brightness split tells you what stands in front.

import uniforms;

[[vk::binding(5, 1)]] Sampler2D sGBufDepth;

/** Mirrors DebugOverlayPushConstants in scenepipeline.cpp. */
struct DebugOverlayPushConstants {
    float4 color;
    /** The render target's extent in pixels, for pixel-sized line widths. */
    float2 resolution;
    /** Half the line width, in pixels. */
    float halfWidth;
    /** Brightness on the occluded side of the depth comparison. */
    float occludedScale;
};
[[vk::push_constant]] DebugOverlayPushConstants pushConstants;

/** The twelve edges of a box, as corner-index pairs. Corner order is the
    AABB class's: (min,max) selected per axis by bits x=1, y=2, z=4. */
static const uint2 kEdges[12] = {
    uint2(0, 1), uint2(2, 3), uint2(4, 5), uint2(6, 7), // x-aligned
    uint2(0, 2), uint2(1, 3), uint2(4, 6), uint2(5, 7), // y-aligned
    uint2(0, 4), uint2(1, 5), uint2(2, 6), uint2(3, 7), // z-aligned
};

struct OverlayVertex {
    float4 position : SV_Position;
    /** Device depth of the edge at this end, interpolated along the quad. */
    float depth : DEPTH;
};

[shader("vertex")]
OverlayVertex overlayVertex(uint vertexId: SV_VertexID) {
    OverlayVertex output;
    const uint edge = vertexId / 6;
    const uint corner = vertexId % 6;

    float3 a = aabbUniforms.corners[kEdges[edge].x].xyz;
    float3 b = aabbUniforms.corners[kEdges[edge].y].xyz;
    float4 clipA = mul(globalUniforms.projection, mul(globalUniforms.view, float4(a, 1.0)));
    float4 clipB = mul(globalUniforms.projection, mul(globalUniforms.view, float4(b, 1.0)));
    // An edge reaching behind the near plane is dropped whole rather than
    // clipped: a debug box with one edge missing reads better than one with a
    // spurious line sweeping across the screen from an inverted projection.
    if (clipA.w < 1e-3 || clipB.w < 1e-3) {
        output.position = float4(0.0, 0.0, -2.0, 1.0);
        output.depth = 1.0;
        return output;
    }
    float3 ndcA = clipA.xyz / clipA.w;
    float3 ndcB = clipB.xyz / clipB.w;

    // The quad: expand perpendicular to the edge in PIXEL space, so the line
    // is the same width at any depth and any direction.
    float2 pxA = (ndcA.xy * float2(0.5, -0.5) + 0.5) * pushConstants.resolution;
    float2 pxB = (ndcB.xy * float2(0.5, -0.5) + 0.5) * pushConstants.resolution;
    float2 dir = pxB - pxA;
    float len = max(length(dir), 1e-3);
    float2 perp = float2(-dir.y, dir.x) / len * pushConstants.halfWidth;

    // corner 0..5 -> two triangles of the quad: A-, A+, B+ / A-, B+, B-.
    const bool atB = corner == 2 || corner == 4 || corner == 5;
    const float side = (corner == 1 || corner == 2 || corner == 4) ? 1.0 : -1.0;
    float2 px = (atB ? pxB : pxA) + perp * side;
    float3 ndc = atB ? ndcB : ndcA;

    float2 xy = (px / pushConstants.resolution - 0.5) * float2(2.0, -2.0);
    output.position = float4(xy, ndc.z, 1.0);
    output.depth = ndc.z;
    return output;
}

[shader("fragment")]
float4 overlayFragment(OverlayVertex input) : SV_Target0 {
    float2 uv = input.position.xy / pushConstants.resolution;
    float sceneDepth = sGBufDepth.SampleLevel(uv, 0.0).r;
    // A small slack keeps an edge lying ON its own surface - a box face
    // coplanar with a wall - reading as visible rather than flickering.
    const float bias = 1e-4;
    bool occluded = input.depth > sceneDepth + bias;
    float3 color = pushConstants.color.rgb * (occluded ? pushConstants.occludedScale : 1.0);
    return float4(color, pushConstants.color.a);
}
