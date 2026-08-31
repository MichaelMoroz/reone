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

// The debug overlay: object bounding boxes and name labels over the finished
// frame, in every render mode. Boxes synthesise their edges from SV_VertexID,
// so there is no vertex buffer and no line topology.
//
// Occlusion is per-pixel against the G-buffer depth rather than a depth test:
// what lies behind geometry keeps its colour and loses opacity, so a hidden box
// still says where its object is.
//
// Projects through viewProjection, not projection: this runs after the temporal
// resolve, and the resolved image is unjittered, so a jittered overlay would
// shimmer against the scene it annotates.

import uniforms;
import lib.text2d;

[[vk::binding(0, 1)]] Sampler2D sFontAtlas;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;

/** Mirrors DebugOverlayPushConstants in scenepipeline.cpp. */
struct DebugOverlayPushConstants {
    float4 color;
    /** Target extent in pixels: both stages work in pixels and convert back. */
    float2 resolution;
    /** Half a line's width, in pixels; the label entry points ignore it. */
    float halfWidth;
    /** Opacity of whatever part of this primitive lies behind geometry. */
    float occludedScale;
    /** World point a label hangs from; the box entry points ignore it. */
    float3 anchor;
};
[[vk::push_constant]] DebugOverlayPushConstants pushConstants;

/** Slack on the depth compare, so a box face coplanar with a wall reads as
    visible rather than flickering between the two states. */
static const float kDepthBias = 1e-4;
/** Below this clip w a projected point is behind the eye and unusable. */
static const float kMinClipW = 1e-4;
/** A position no raster can produce, for a primitive that must not appear. */
static const float4 kCulledPosition = float4(0.0, 0.0, -2.0, 1.0);

/**
 * The overlay's one occlusion rule: full opacity in front of the scene,
 * scaled down behind it, decided per pixel.
 */
float overlayOpacity(float2 fragmentPosition, float depth) {
    float2 uv = fragmentPosition / pushConstants.resolution;
    float sceneDepth = sGBufDepth.SampleLevel(uv, 0.0).r;
    return depth > sceneDepth + kDepthBias ? pushConstants.occludedScale : 1.0;
}

/** Clip space to pixels, with clip-space y up and screen y down. */
float2 clipToPixels(float3 ndc) {
    return (ndc.xy * float2(0.5, -0.5) + 0.5) * pushConstants.resolution;
}

/** Pixels back to the clip-space xy a vertex is emitted in. */
float2 pixelsToClip(float2 pixels) {
    return (pixels / pushConstants.resolution - 0.5) * float2(2.0, -2.0);
}

// --- Boxes ---

/** The twelve edges of a box, as corner-index pairs. Corner i selects max
    over min per axis by bits x=1, y=2, z=4, matching DebugOverlayShape. */
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

OverlayVertex culledEdge() {
    OverlayVertex output;
    output.position = kCulledPosition;
    output.depth = 1.0;
    return output;
}

/** One world-space segment as a screen-space quad, six vertices. */
OverlayVertex segmentVertex(float3 a, float3 b, uint corner) {
    float4 clipA = mul(globalUniforms.viewProjection, float4(a, 1.0));
    float4 clipB = mul(globalUniforms.viewProjection, float4(b, 1.0));

    // Clip to the near plane (z = 0 in Vulkan), don't drop: a camera inside a
    // box has eight of its twelve edges ending behind the eye.
    if (clipA.z <= 0.0 && clipB.z <= 0.0) {
        return culledEdge();
    }
    if (clipA.z < 0.0) {
        clipA = lerp(clipA, clipB, clipA.z / (clipA.z - clipB.z));
    } else if (clipB.z < 0.0) {
        clipB = lerp(clipB, clipA, clipB.z / (clipB.z - clipA.z));
    }
    if (clipA.w < kMinClipW || clipB.w < kMinClipW) {
        return culledEdge();
    }
    float3 ndcA = clipA.xyz / clipA.w;
    float3 ndcB = clipB.xyz / clipB.w;

    // The quad expands perpendicular to the edge in PIXEL space, so a line is
    // the same width at any depth and in any direction.
    float2 pxA = clipToPixels(ndcA);
    float2 pxB = clipToPixels(ndcB);
    float2 direction = pxB - pxA;
    float screenLength = max(length(direction), 1e-3);
    float2 perpendicular =
        float2(-direction.y, direction.x) / screenLength * pushConstants.halfWidth;

    // corner 0..5 -> two triangles of the quad: A-, A+, B+ / A-, B+, B-.
    const bool atB = corner == 2 || corner == 4 || corner == 5;
    const float side = (corner == 1 || corner == 2 || corner == 4) ? 1.0 : -1.0;
    float2 px = (atB ? pxB : pxA) + perpendicular * side;
    float3 ndc = atB ? ndcB : ndcA;

    OverlayVertex output;
    output.position = float4(pixelsToClip(px), ndc.z, 1.0);
    output.depth = ndc.z;
    return output;
}

[shader("vertex")]
OverlayVertex overlayVertex(uint vertexId: SV_VertexID) {
    const uint edge = vertexId / 6;
    return segmentVertex(aabbUniforms.corners[kEdges[edge].x].xyz,
                         aabbUniforms.corners[kEdges[edge].y].xyz,
                         vertexId % 6);
}

/** A free line from the aabb block's first two corners: it already carries eight. */
[shader("vertex")]
OverlayVertex overlayLineVertex(uint vertexId: SV_VertexID) {
    return segmentVertex(aabbUniforms.corners[0].xyz,
                         aabbUniforms.corners[1].xyz,
                         vertexId);
}

[shader("fragment")]
float4 overlayFragment(OverlayVertex input) : SV_Target0 {
    float opacity = overlayOpacity(input.position.xy, input.depth);
    return float4(pushConstants.color.rgb, pushConstants.color.a * opacity);
}

// --- Labels ---

/**
 * The translucent backing of one label block. The rect arrives in pixels
 * relative to the anchor through the text block's first glyph slot - the
 * block is otherwise unused by this entry point.
 */
[shader("vertex")]
OverlayVertex overlayTextBackgroundVertex(uint vertexId: SV_VertexID) {
    float4 clip = mul(globalUniforms.viewProjection, float4(pushConstants.anchor, 1.0));
    if (clip.w < kMinClipW) {
        return culledEdge();
    }
    float3 ndc = clip.xyz / clip.w;
    float2 anchorPx = floor(clipToPixels(ndc) + 0.5);
    float4 rect = textUniforms.chars[0].posScale;
    float2 corner = quadCorner(vertexId);
    OverlayVertex output;
    output.position =
        float4(pixelsToClip(anchorPx + rect.xy + corner * rect.zw), saturate(ndc.z), 1.0);
    output.depth = ndc.z;
    return output;
}

struct OverlayGlyph {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float4 glyphUV : TEXCOORD1;
    /** The anchor's device depth: one label, one depth, as a billboard has. */
    nointerpolation float depth : DEPTH;
};

[shader("vertex")]
OverlayGlyph overlayTextVertex(uint vertexId: SV_VertexID, uint glyph: SV_InstanceID) {
    OverlayGlyph output;
    float4 clip = mul(globalUniforms.viewProjection, float4(pushConstants.anchor, 1.0));
    if (clip.w < kMinClipW) {
        output.position = kCulledPosition;
        output.uv = float2(0.0);
        output.glyphUV = float4(0.0);
        output.depth = 1.0;
        return output;
    }
    float3 ndc = clip.xyz / clip.w;
    // Snapped to whole pixels: a bitmap atlas drawn at 1:1 has to land ON the
    // texel grid, or every glyph samples between texels and blurs.
    float2 anchorPx = floor(clipToPixels(ndc) + 0.5);

    float2 corner = quadCorner(vertexId);

    float4 posScale = textUniforms.chars[glyph].posScale;
    float4 glyphUV = textUniforms.chars[glyph].uv;

    output.position = float4(
        pixelsToClip(anchorPx + posScale.xy + corner * posScale.zw), saturate(ndc.z), 1.0);
    output.uv = glyphUV.xy + quadUV(corner) * glyphUV.zw;
    output.glyphUV = glyphUV;
    output.depth = ndc.z;
    return output;
}

/** vk2d.slang's text fragment plus the overlay's occlusion rule. */
[shader("fragment")]
float4 overlayTextFragment(OverlayGlyph input) : SV_Target0 {
    float4 glyph = glyphTexel(sFontAtlas, input.uv, input.glyphUV);
    float opacity = overlayOpacity(input.position.xy, input.depth);
    return float4(pushConstants.color.rgb * glyph.rgb, pushConstants.color.a * glyph.a * opacity);
}
