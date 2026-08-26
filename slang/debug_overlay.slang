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
// the edge's own depth against the G-buffer depth at the pixel and makes the
// occluded part SEMI-TRANSPARENT rather than discarding it - the hue stays,
// the scene shows through, so a hidden box still tells you where the object
// is and the opacity split tells you what stands in front.

import uniforms;

[[vk::binding(0, 1)]] Sampler2D sFontAtlas;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;

/** Mirrors DebugOverlayPushConstants in scenepipeline.cpp. */
struct DebugOverlayPushConstants {
    float4 color;
    /** The render target's extent in pixels, for pixel-sized line widths. */
    float2 resolution;
    /** Half the line width, in pixels. */
    float halfWidth;
    /** Opacity factor on the occluded side of the depth comparison. */
    float occludedScale;
    /** World anchor a text label hangs from; unused by the box entries. */
    float3 anchor;
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
    // Clip the edge to the near plane rather than dropping it whole. Dropping
    // collapsed every box the camera stands INSIDE to its far face - eight of
    // the twelve edges have a corner behind the view there - which read as a
    // flat 2D rectangle instead of a room volume. Vulkan clip space puts the
    // near plane at z = 0, so the surviving portion is the z >= 0 span.
    if (clipA.z <= 0.0 && clipB.z <= 0.0) {
        output.position = float4(0.0, 0.0, -2.0, 1.0);
        output.depth = 1.0;
        return output;
    }
    if (clipA.z < 0.0) {
        clipA = lerp(clipA, clipB, clipA.z / (clipA.z - clipB.z));
    } else if (clipB.z < 0.0) {
        clipB = lerp(clipB, clipA, clipB.z / (clipB.z - clipA.z));
    }
    if (clipA.w < 1e-4 || clipB.w < 1e-4) {
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
    // Occluded segments keep their full hue and lose OPACITY: the pass alpha
    // blends, so the scene shows through the hidden part of the line instead
    // of the line going dark over it.
    float alpha = pushConstants.color.a * (occluded ? pushConstants.occludedScale : 1.0);
    return float4(pushConstants.color.rgb, alpha);
}


// --- Labels ---
//
// The same pass, the same depth rule. A label is a screen-space run of glyph
// quads hung off a world anchor: the vertex stage projects the anchor once and
// lays the glyphs out around it in pixels (the layout itself is the 2D
// renderer's, filled into the shared text block), and the fragment stage
// samples the G-buffer depth exactly as the box edges do. Text that has fallen
// behind geometry therefore goes translucent with its lines instead of
// floating over the scene as though nothing were in front of it.

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
    float4 clip = mul(globalUniforms.projection,
                      mul(globalUniforms.view, float4(pushConstants.anchor, 1.0)));
    if (clip.w < 1e-4) {
        output.position = float4(0.0, 0.0, -2.0, 1.0);
        output.uv = float2(0.0);
        output.glyphUV = float4(0.0);
        output.depth = 1.0;
        return output;
    }
    float3 ndc = clip.xyz / clip.w;
    // Snapped to whole pixels. A bitmap atlas drawn at 1:1 has to land ON the
    // texel grid; an anchor at a fractional pixel makes every glyph sample
    // between texels, which is what turned these labels to mush.
    float2 anchorPx = floor((ndc.xy * float2(0.5, -0.5) + 0.5) *
                            pushConstants.resolution + 0.5);

    // Triangles (0,1,2) and (1,2,3) over corners 0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1),
    // and the v flip that pairs position (0,0) with uv (0,1) - the same pairing
    // vk2d.slang uses, or every glyph comes out upside down.
    uint cornerId = (vertexId < 3) ? vertexId : vertexId - 2;
    float2 corner = float2(cornerId & 1, (cornerId >> 1) & 1);

    float4 posScale = textUniforms.chars[glyph].posScale;
    float4 uv = textUniforms.chars[glyph].uv;
    float2 px = anchorPx + posScale.xy + corner * posScale.zw;

    float2 xy = (px / pushConstants.resolution - 0.5) * float2(2.0, -2.0);
    output.position = float4(xy, saturate(ndc.z), 1.0);
    output.uv = uv.xy + float2(corner.x, 1.0 - corner.y) * uv.zw;
    output.glyphUV = uv;
    output.depth = ndc.z;
    return output;
}

/**
 * Coverage reconstruction while magnifying cutout artwork, copied from
 * vk2d.slang: at native size the stored coverage is already the best signal
 * and thresholding it would only throw detail away.
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

[shader("fragment")]
float4 overlayTextFragment(OverlayGlyph input) : SV_Target0 {
    // This IS vk2d.slang's textFragment - the same atlas, the same half-texel
    // clamp against neighbouring glyphs, the same coverage sharpening, the
    // same colour multiply - with exactly one thing added: the depth compare
    // the rest of this overlay makes. Nothing here reshapes a glyph. An
    // outline and a threshold of my own went in first and closed every
    // counter in the font: measured at zero holes in 141,753 inked pixels,
    // which is why a, o and 0 had become the same solid block.
    uint width, height;
    sFontAtlas.GetDimensions(width, height);
    float2 halfTexel = 0.5 / float2(width, height);
    float2 inset = min(halfTexel, 0.5 * input.glyphUV.zw);
    float2 uv = clamp(input.uv, input.glyphUV.xy + inset,
                      input.glyphUV.xy + input.glyphUV.zw - inset);
    float4 atlas = sFontAtlas.Sample(uv);
    float alpha = sharpenMagnifiedAlpha(sFontAtlas, uv, atlas.a);

    float2 screenUv = input.position.xy / pushConstants.resolution;
    float sceneDepth = sGBufDepth.SampleLevel(screenUv, 0.0).r;
    const float bias = 1e-4;
    bool occluded = input.depth > sceneDepth + bias;
    return float4(pushConstants.color.rgb * atlas.rgb,
                  pushConstants.color.a * alpha *
                      (occluded ? pushConstants.occludedScale : 1.0));
}
