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
// Screen-space drawing: sprites, filled rectangles, text and full-target
// images. The Vulkan half of I2DRenderer.
//
// No vertex buffers. Every primitive here is a quad, and a quad is cheaper to
// synthesise from SV_VertexID than to bind a buffer for. Text goes further and
// draws one instance per glyph, reading position and atlas coordinates from the
// text uniform block, which is what the OpenGL path does too.

import uniforms;

[[vk::binding(0, 1)]] Sampler2D sMainTex;

/** Corner of a unit quad, 0..1, from a six-vertex triangle list. */
float2 quadCorner(uint vertexId) {
    // Triangles (0,1,2) and (1,2,3) over corners 0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1).
    uint corner = (vertexId < 3) ? vertexId : vertexId - 2;
    return float2(corner & 1, (corner >> 1) & 1);
}

/**
 * Texture coordinate for a corner.
 *
 * The OpenGL quad mesh pairs position (0,0) with uv (0,1) - v runs upwards
 * while screen y runs down - so the same pairing has to hold here or every
 * sprite, glyph and panel comes out upside down.
 */
float2 quadUV(float2 corner) {
    return float2(corner.x, 1.0f - corner.y);
}

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

/**
 * A quad placed by localUniforms.model and projected by
 * globalUniforms.projection, which the caller sets to a screen-space ortho.
 */
[shader("vertex")]
Varyings quadVertex(uint vertexId: SV_VertexID) {
    float2 corner = quadCorner(vertexId);

    Varyings output;
    output.position = mul(globalUniforms.projection,
                          mul(localUniforms.model, float4(corner, 0.0f, 1.0f)));
    // localUniforms.uv is the same 3x4 the GL path uses to flip or sub-rect.
    output.uv = mul(float3(quadUV(corner), 1.0f), float3x2(localUniforms.uv[0].xy,
                                                           localUniforms.uv[1].xy,
                                                           localUniforms.uv[2].xy));
    return output;
}

[shader("fragment")]
float4 imageFragment(Varyings input) : SV_Target {
    return sMainTex.Sample(input.uv) * localUniforms.color;
}

[shader("fragment")]
float4 colorFragment(Varyings input) : SV_Target {
    return localUniforms.color;
}

/** A quad covering the whole target, bypassing the 2D projection. */
[shader("vertex")]
Varyings fullTargetVertex(uint vertexId: SV_VertexID) {
    float2 corner = quadCorner(vertexId);

    Varyings output;
    output.position = float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = mul(float3(quadUV(corner), 1.0f), float3x2(localUniforms.uv[0].xy,
                                                           localUniforms.uv[1].xy,
                                                           localUniforms.uv[2].xy));
    return output;
}

/**
 * One instance per glyph. posScale is the glyph's screen rect and uv its rect
 * in the atlas, both filled in by the renderer from the font's metrics.
 */
[shader("vertex")]
Varyings textVertex(uint vertexId: SV_VertexID, uint glyph: SV_InstanceID) {
    float2 corner = quadCorner(vertexId);
    float4 posScale = textUniforms.chars[glyph].posScale;
    float4 uv = textUniforms.chars[glyph].uv;

    Varyings output;
    output.position = mul(globalUniforms.projection,
                          float4(posScale.xy + corner * posScale.zw, 0.0f, 1.0f));
    // Same v convention as the sprites: the glyph rect starts at the atlas
    // bottom and grows upwards, while screen y grows downwards.
    output.uv = uv.xy + quadUV(corner) * uv.zw;
    return output;
}

[shader("fragment")]
float4 textFragment(Varyings input) : SV_Target {
    return sMainTex.Sample(input.uv) * localUniforms.color;
}
