/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The post-denoise assembly uses the guide factors that accompanied the
// demodulated radiance through NRD, overwriting the trace kernel's own write.
//
// One job, and one wrinkle. The denoised channels only exist inside NRD's
// denoising range - REBLUR never writes its outputs at sky depth, so adding
// them there splashed uninitialized pool memory across the sky. A plain sky
// miss has only noise-free radiance, but a transmitting surface in front of
// the sky leaves real radiance in the noisy channels with no guide surface for
// NRD. The viewZ guard therefore falls back to those raw channels.
//
// Anti-aliasing is not this pass's problem. It used to run a hand-rolled TAA
// here, reprojecting through the same motion vectors NRD uses and clamping
// against a 3x3 neighbourhood; FSR does that job properly and measured 4.6x
// better on edges, so the history, the reprojection and the clamp are gone
// rather than left switched off.
//
// The output is linear HDR, unconditionally. The display transform belongs
// downstream of the upscaler that may sit after this pass, and downstream of
// transparency, so it is the common post-process pass's and no dial of it
// reaches here.

[[vk::binding(0, 1)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] RWTexture2D<float4> inNoiseFree;
[[vk::binding(2, 1)]] RWTexture2D<float4> inDiffFactor;
[[vk::binding(3, 1)]] RWTexture2D<float4> inSpecFactor;
[[vk::binding(4, 1)]] RWTexture2D<float4> inDenoisedDiffuse;
[[vk::binding(5, 1)]] RWTexture2D<float4> inDenoisedSpecular;
[[vk::binding(6, 1)]] RWTexture2D<float> inViewZ;
[[vk::binding(7, 1)]] RWTexture2D<float4> inRawDiffuse;
[[vk::binding(8, 1)]] RWTexture2D<float4> inRawSpecular;

/** Denoised channels are undefined beyond NRD's denoising range. */
static const float kDenoisedRange = 1.0e5;

float3 yCoCgToLinear(float3 color) {
    float t = color.x - color.z * 0.5;
    float g = color.z + t;
    float b = t - color.y * 0.5;
    return max(float3(b + color.y, g, b), 0.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;
    int2 pixel = int2(dispatchId.xy);

    float3 color = inNoiseFree[pixel].rgb;
    bool hasDenoisedSurface = abs(inViewZ[pixel]) < kDenoisedRange;
    float3 diffuse = hasDenoisedSurface ? inDenoisedDiffuse[pixel].rgb
                                        : inRawDiffuse[pixel].rgb;
    float3 specular = hasDenoisedSurface ? inDenoisedSpecular[pixel].rgb
                                         : inRawSpecular[pixel].rgb;
    color += yCoCgToLinear(diffuse) * inDiffFactor[pixel].rgb +
             yCoCgToLinear(specular) * inSpecFactor[pixel].rgb;
    outputImage[pixel] = float4(color, 1.0);
}
