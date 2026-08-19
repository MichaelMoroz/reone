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
// The sky's own two small programs.
//
// The first is the sky-room capture that bakes the cube. It is deliberately a
// complete, tiny shader program rather than another material-pipeline entry
// point: the sky is unlit and its original mesh buffers provide only the
// position and primary UV it needs.
//
// The second is the motion the sky owes the temporal resolve, below. It is not
// the composite pass coming back - it writes no colour and reads no cube - but
// it lives here because "what the sky does at an uncovered pixel" is one
// subject, and the direction it reprojects is the same one the resolves shade.

import uniforms;
import lib.skycube;

[[vk::binding(0, 1)]] Sampler2D sMainTex;
[[vk::binding(21, 1), vk::image_format("r32ui")]] Sampler2D<uint> sGBufTriangleId;

// Restated rather than imported, as pbr_resolve, retro_resolve, postprocess and
// debug_view all restate it: lib/gbuf.slang holds the G-buffer packing but not
// this, and exporting it from there makes the name ambiguous in every module
// that already carries its own. Mirrors GBuffer::kNoTriangle, which is what
// scenepipeline.cpp actually clears the target to.
static const uint kNoTriangle = 0xffffffffu;

struct SkyVertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct SkyVaryings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
SkyVaryings skyVertex(SkyVertexInput input) {
    SkyVaryings output;
    float4 worldPosition = mul(localUniforms.model, float4(input.position, 1.0));
    output.position = mul(globalUniforms.projection,
                          mul(globalUniforms.view, worldPosition));
    output.uv = input.uv;
    return output;
}

float2 transformedUV(float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(
        localUniforms.uv[0].x * h.x + localUniforms.uv[1].x * h.y + localUniforms.uv[2].x * h.z,
        localUniforms.uv[0].y * h.x + localUniforms.uv[1].y * h.y + localUniforms.uv[2].y * h.z);
}

[shader("fragment")]
float4 skyFragment(SkyVaryings input) : SV_Target {
    float4 sample = sMainTex.Sample(transformedUV(input.uv));
    if (sample.a == 0.0) discard;
    // The cube is sampled as radiance by the tracer. Preserve the old bake's
    // explicit gamma conversion exactly.
    return float4(pow(sample.rgb, float3(2.2)), sample.a);
}

struct SkyMotionVaryings {
    float4 position : SV_Position;
    noperspective float2 uv : TEXCOORD0;
};

[shader("vertex")]
SkyMotionVaryings skyMotionVertex(uint vertexId : SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    SkyMotionVaryings output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

/**
 * The motion vector at a pixel nothing was drawn over.
 *
 * The geometry pass clears the motion target and then writes only where a
 * fragment landed, so before this pass the sky reported that it had not moved -
 * and a temporal resolve believes that. Panning the camera therefore dragged
 * last frame's sky along with the new one and left the whole background
 * smeared, on exactly the pixels with no other signal to correct it.
 *
 * Written as a discard rather than a blend: the covered pixels already hold the
 * surface motion the geometry pass computed, and this pass has nothing to add
 * to them. The triangle-id sentinel selects the same set both resolves hand to
 * the sky, so the pixel that gets sky motion here is the pixel that gets sky
 * radiance there - one definition of "uncovered", not two.
 */
[shader("fragment")]
float2 skyMotionFragment(SkyMotionVaryings input) : SV_Target0 {
    uint triangleId = sGBufTriangleId.Load(int3(int2(input.position.xy), 0)).r;
    if (triangleId != kNoTriangle) {
        discard;
    }
    float3 direction = skyDirection(input.uv, globalUniforms.projectionInv,
                                    globalUniforms.viewInv,
                                    globalUniforms.cameraPosition.xyz);
    return skyMotion(direction, globalUniforms.viewProjection,
                     globalUniforms.prevViewProjection);
}

// The composite that used to live here is gone, and with it the pass that ran
// it. Splitting it out was meant to stop two resolves each implementing the
// sky; a shared function - lib/skycube.slang, which the tracer reads too - is
// one implementation and costs neither a full-target load/store nor a pipeline
// and descriptor set of its own. Both resolves now call it where they detect
// an uncovered pixel, which is the same set of pixels the composite's far-depth
// test selected: the material id and the depth are written by one fragment.
