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

// The debug overlay: an object's bounding box and its name, over the finished
// frame, in every render mode.
//
// Two entry-point pairs, one rule. The boxes synthesise twelve edges from
// SV_VertexID - six vertices each, a screen-space quad two half-widths wide -
// so no vertex buffer and no line topology exist; the labels are runs of glyph
// quads hung off a world anchor, laid out by the same code that lays out any
// other run of text and sampled from the same font atlas.
//
// The rule both share is overlayOpacity: occlusion is decided per pixel
// against the G-buffer depth rather than by a depth test, and whatever lies
// behind geometry keeps its colour and loses OPACITY. A hidden box still says
// where its object is, the scene shows through it, and the difference in
// opacity is what says which one is in front.
//
// It runs after the display transform, so nothing here is graded, anti-aliased
// or sharpened - the overlay is a diagnostic drawn ON the picture, not part of
// it.
//
// Which is exactly why both stages project through globalUniforms.viewProjection
// and not globalUniforms.projection: the latter carries the temporal resolve's
// sub-pixel jitter, and a pass drawn AFTER that resolve must not. The resolved
// image is the unjittered one, so a jittered overlay would sit a fraction of a
// pixel off the scene it annotates and shimmer as the offset cycled. Draw this
// before the anti-aliasing slot instead and the jitter would be correct - but
// then the boxes would be temporally resolved and tonemapped along with the
// picture, which is what this pass exists to avoid.

import uniforms;

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

/**
 * One world-space segment as a screen-space quad, six vertices.
 *
 * Shared by the box entry below - which feeds it an AABB edge - and by the
 * free-line entry, which feeds it the two points a caller supplied. Keeping
 * one implementation is what makes a pathfinder line and a bounding-box edge
 * behave identically at the near plane and at grazing angles.
 */
OverlayVertex segmentVertex(float3 a, float3 b, uint corner) {
    float4 clipA = mul(globalUniforms.viewProjection, float4(a, 1.0));
    float4 clipB = mul(globalUniforms.viewProjection, float4(b, 1.0));

    // Clipped to the near plane, not dropped whole. Dropping any edge with a
    // corner behind the eye collapses every box the camera stands INSIDE to
    // its far face - eight of the twelve edges end behind the view there - and
    // a room then draws as a flat rectangle instead of a volume. Vulkan clip
    // space puts the near plane at z = 0, so the surviving span is z >= 0.
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

/**
 * A free line, drawn from the first two corner slots.
 *
 * It rides the same aabb block rather than a block of its own: a segment needs
 * two points and that block already carries eight, so the whole descriptor and
 * uniform-ring path is reused unchanged and the draw is six vertices instead
 * of seventy-two.
 */
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

    // Triangles (0,1,2) and (1,2,3) over corners 0=(0,0) 1=(1,0) 2=(0,1)
    // 3=(1,1), with the v flip that pairs position (0,0) with uv (0,1) - the
    // same pairing vk2d.slang uses, or every glyph comes out upside down.
    uint cornerId = (vertexId < 3) ? vertexId : vertexId - 2;
    float2 corner = float2(cornerId & 1, (cornerId >> 1) & 1);

    float4 posScale = textUniforms.chars[glyph].posScale;
    float4 glyphUV = textUniforms.chars[glyph].uv;

    output.position = float4(
        pixelsToClip(anchorPx + posScale.xy + corner * posScale.zw), saturate(ndc.z), 1.0);
    output.uv = glyphUV.xy + float2(corner.x, 1.0 - corner.y) * glyphUV.zw;
    output.glyphUV = glyphUV;
    output.depth = ndc.z;
    return output;
}

/**
 * Coverage reconstruction while magnifying cutout artwork, as vk2d.slang does
 * it: at native size the stored coverage is already the best signal and
 * thresholding it would only throw detail away.
 */
float sharpenMagnifiedAlpha(Sampler2D image, float2 uv, float alpha) {
    uint width, height;
    image.GetDimensions(width, height);
    float2 sourceFootprint = fwidth(uv) * float2(width, height);
    float largestFootprint = max(sourceFootprint.x, sourceFootprint.y);
    float sharpenAmount = 1.0 - smoothstep(0.75, 1.0, largestFootprint);

    float alphaWidth = max(0.5 * fwidth(alpha), 1.0 / 255.0);
    float sharpened = smoothstep(0.5 - alphaWidth, 0.5 + alphaWidth, alpha);
    return lerp(alpha, sharpened, sharpenAmount);
}

/**
 * This is vk2d.slang's textFragment - the same atlas, the same half-texel
 * clamp against the neighbouring glyph in the sheet, the same coverage
 * sharpening, the same colour multiply - with the overlay's occlusion rule as
 * the only addition.
 *
 * Do not reshape a glyph here. An outline or a threshold of its own closes
 * this font's one-texel counters, and a, o and 0 become the same solid block.
 */
[shader("fragment")]
float4 overlayTextFragment(OverlayGlyph input) : SV_Target0 {
    uint width, height;
    sFontAtlas.GetDimensions(width, height);
    float2 halfTexel = 0.5 / float2(width, height);
    float2 inset = min(halfTexel, 0.5 * input.glyphUV.zw);
    float2 uv = clamp(input.uv, input.glyphUV.xy + inset,
                      input.glyphUV.xy + input.glyphUV.zw - inset);
    float4 atlas = sFontAtlas.Sample(uv);
    float coverage = sharpenMagnifiedAlpha(sFontAtlas, uv, atlas.a);

    float opacity = overlayOpacity(input.position.xy, input.depth);
    return float4(pushConstants.color.rgb * atlas.rgb,
                  pushConstants.color.a * coverage * opacity);
}
