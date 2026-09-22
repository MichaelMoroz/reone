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

// Post-denoise assembly, using the guide factors that accompanied the
// demodulated radiance through NRD.
//
// Direct diffuse at the primary vertex bypasses NRD: a filter wide enough to
// settle bounce noise softens contact shadows, so it is stratified at the
// source and left to the temporal resolve.
//
// NRD writes nothing outside its denoising range, so the viewZ guard falls back
// to the raw channels at sky depth - a transmitting surface in front of the sky
// leaves real radiance there with no guide surface.
//
// Output is linear HDR unconditionally; the display transform belongs to the
// post-process pass downstream of the upscaler.

import lib.assemble;

[[vk::binding(0, 1)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] RWTexture2D<float4> inNoiseFree;
[[vk::binding(2, 1)]] RWTexture2D<float4> inDiffFactor;
[[vk::binding(3, 1)]] RWTexture2D<float4> inSpecFactor;
// Sampled, not storage: read at a sub-pixel offset through the filtering unit.
[[vk::binding(4, 1)]] Sampler2D sDenoisedDiffuse;
[[vk::binding(5, 1)]] Sampler2D sDenoisedSpecular;
[[vk::binding(6, 1)]] RWTexture2D<float> inViewZ;
[[vk::binding(7, 1)]] RWTexture2D<float4> inRawDiffuse;
[[vk::binding(8, 1)]] RWTexture2D<float4> inRawSpecular;
// Direct diffuse at the primary vertex, raw or denoised, in the same currency as the rest.
[[vk::binding(9, 1)]] Sampler2D sDirectDiffuse;

/** Mirrors CompositePushConstants in scenepipeline.cpp. */
struct CompositePushConstants {
    /** Sub-pixel offset the denoised channels are read at, so they line up with the guides.
        Two scalars: a float2 would realign the block and break the C++ mirror. */
    float denoisedJitterX;
    float denoisedJitterY;
    /** A post-denoise debug channel this pass owns, or 0. */
    uint debugView;
    /** Only a denoised direct channel is read at the offset; raw still carries the trace's jitter. */
    uint directDenoised;
};

static const uint kDebugDenoisedDiffuse = 17;
static const uint kDebugDenoisedSpecular = 18;
static const uint kDebugFilteredDirect = 19;
[[vk::push_constant]] CompositePushConstants pushConstants;

/** This pixel's centre, displaced by the offset the raster beside it was sampled through. */
float2 denoisedUv(int2 pixel, float2 extent) {
    return (float2(pixel) + 0.5 +
            float2(pushConstants.denoisedJitterX, pushConstants.denoisedJitterY)) /
           extent;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;
    int2 pixel = int2(dispatchId.xy);

    const float2 extent = float2(width, height);
    const float2 centreUv = (float2(pixel) + 0.5) / extent;
    // Sky and any path-terminating surface carry all of their radiance in the
    // noise-free channel; beyond NRD's range its outputs are stale history, not
    // zero, so nothing denoised is assembled onto them.
    const bool hasDenoisedSurface = !isTerminatingSurface(inViewZ[pixel]);

    // Demodulated, deliberately: a channel multiplied by its albedo no longer
    // shows its own noise. Read at the pixel centre - what the denoiser produced.
    // RELAX hands back linear radiance; the clamp is against an edge-stopping
    // filter's small negatives.
    switch (pushConstants.debugView) {
    case kDebugDenoisedDiffuse:
        outputImage[pixel] = float4(max(hasDenoisedSurface ? sDenoisedDiffuse.SampleLevel(centreUv, 0.0).rgb
                                                           : inRawDiffuse[pixel].rgb, 0.0), 1.0);
        return;
    case kDebugDenoisedSpecular:
        outputImage[pixel] = float4(max(hasDenoisedSurface ? sDenoisedSpecular.SampleLevel(centreUv, 0.0).rgb
                                                           : inRawSpecular[pixel].rgb, 0.0), 1.0);
        return;
    case kDebugFilteredDirect:
        outputImage[pixel] = float4(max(sDirectDiffuse.SampleLevel(centreUv, 0.0).rgb, 0.0), 1.0);
        return;
    default:
        break;
    }

    float3 color = inNoiseFree[pixel].rgb;
    if (hasDenoisedSurface) {
        // NRD suppresses jitter, so the denoised channels sit on the pixel
        // centre while the guide factors were sampled jittered; reading them at
        // the same offset closes the mismatch, which otherwise freezes into a
        // static aliased pattern the upscaler cannot reconstruct. Sampler
        // interpolation, and the offset is under half a pixel.
        const float2 jitteredUv = denoisedUv(pixel, extent);
        const float3 direct = sDirectDiffuse
                                  .SampleLevel(pushConstants.directDenoised != 0 ? jitteredUv : centreUv, 0.0)
                                  .rgb;
        color = assembleRadiance(color, max(sDenoisedDiffuse.SampleLevel(jitteredUv, 0.0).rgb, 0.0),
                                 max(direct, 0.0),
                                 max(sDenoisedSpecular.SampleLevel(jitteredUv, 0.0).rgb, 0.0),
                                 inDiffFactor[pixel].rgb, inSpecFactor[pixel].rgb);
    }
    outputImage[pixel] = float4(color, 1.0);
}
