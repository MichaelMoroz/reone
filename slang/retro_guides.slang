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

// The two surface guides DLSS Ray Reconstruction requires and retro does not
// otherwise produce.
//
// Retro shades forward out of the G-buffer and never fills the tracing
// channels, so the guides the traced and PBR providers hand to RR do not exist
// in this mode. RR without them does not degrade - slEvaluateFeature returns
// eErrorMissingInputParameter and the frame is never resolved at all - so this
// pass is what makes the mode selectable rather than an improvement to it.
//
// The third guide, albedo, needs no pass: the G-buffer's diffuse attachment IS
// the albedo retro's resolve multiplies its lighting by, so the pipeline tags
// that image directly.

import lib.gbuf;

[[vk::binding(0, 1)]] RWTexture2D<float4> outNormalRoughness;
[[vk::binding(1, 1)]] RWTexture2D<float4> outSpecularAlbedo;
// Sampled rather than storage: the G-buffer's attachments are created without
// storage usage, and this only ever reads the texel under the pixel.
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;

struct RetroGuidePushConstants {
    float4x4 viewInv;
    // One uint4 rather than two loose uints, and only zw are spare. A matrix
    // aligns its successor to 16 bytes, so a trailing pair of uints reflects as
    // an 80-byte block while the C++ struct beside it measures 72 - and the
    // dispatch's size check turns that mismatch into an exception thrown mid
    // recording. Naming the padding keeps the two definitions the same size by
    // construction rather than by arithmetic nobody redoes.
    uint4 extent;
};

[[vk::push_constant]] RetroGuidePushConstants pushConstants;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    if (dispatchId.x >= pushConstants.extent.x || dispatchId.y >= pushConstants.extent.y) {
        return;
    }
    int2 pixel = int2(dispatchId.xy);

    // The same decode retroResolveFragment does, to the same world space: RR is
    // configured for packed world normals, and an eye-space normal would make
    // every reprojection judgement it makes a function of where the camera was
    // looking. Keep the two in step if either changes.
    float3 eyeNormal = gbufDecodeEyeNormal(sGBufEyeNormal.Load(int3(pixel, 0)).xyz);
    float3 normal = normalize(mul(pushConstants.viewInv, float4(eyeNormal, 0.0)).xyz);

    // Fully rough, and specular-black.
    //
    // Retro has no microfacet model to report. Its only view-dependent term is
    // the authored mirror - a static cube added on top of the shaded colour,
    // not a lobe with an albedo - so there is no specular signal for RR to
    // demodulate and reproject. Declaring none is the accurate statement;
    // inventing a lobe would have RR reproject the diffuse image as though
    // part of it moved with the camera.
    outNormalRoughness[pixel] = float4(normal, 1.0);
    outSpecularAlbedo[pixel] = float4(0.0, 0.0, 0.0, 1.0);
}
