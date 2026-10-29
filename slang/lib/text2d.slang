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

// Screen-space quads and bitmap-font glyphs, shared by the 2D renderer and the
// debug overlay: the same corner and uv convention, the same coverage
// sharpening, the same half-texel clamp against the neighbouring glyph.

module text2d;

/** Corner of a unit quad, 0..1, from a six-vertex triangle list: (0,1,2) and (1,2,3). */
public float2 quadCorner(uint vertexId) {
    uint corner = (vertexId < 3) ? vertexId : vertexId - 2;
    return float2(corner & 1, (corner >> 1) & 1);
}

/** Texture coordinate for a corner: v runs upwards while screen y runs down. */
public float2 quadUV(float2 corner) {
    return float2(corner.x, 1.0 - corner.y);
}

/** Coverage sharpening, only while magnifying; at native size the stored coverage is best. */
public float sharpenMagnifiedAlpha(Sampler2D image, float2 uv, float alpha) {
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
 * A glyph's texel: clamped half a texel inside its atlas rect so the
 * neighbouring glyph never bleeds in, with sharpened coverage in alpha. Do not
 * reshape a glyph beyond this - an outline or threshold closes the font's
 * one-texel counters.
 */
public float4 glyphTexel(Sampler2D atlas, float2 uv, float4 glyphUV) {
    uint width, height;
    atlas.GetDimensions(width, height);
    float2 halfTexel = 0.5 / float2(width, height);
    float2 inset = min(halfTexel, 0.5 * glyphUV.zw);
    float2 clamped = clamp(uv, glyphUV.xy + inset, glyphUV.xy + glyphUV.zw - inset);
    float4 texel = atlas.Sample(clamped);
    return float4(texel.rgb, sharpenMagnifiedAlpha(atlas, clamped, texel.a));
}
