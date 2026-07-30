/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The display transform, split out so it can run AFTER the upscaler.
//
// Without FSR the composite tonemaps as its last act. With FSR it cannot: the
// upscaler wants linear pre-tonemap colour, and AMD places tonemapping in the
// "after upscaling" column alongside bloom and film grain, for the same reason
// grain goes there - anything applied before is resampled and amplified by the
// temporal accumulation. So the composite stops at linear HDR, FSR resolves,
// and this pass finishes the frame at presentation resolution.
//
// FSR returns its result in the same domain as its input: it multiplies by the
// exposure it was given, accumulates, and divides it back out before writing.
// The colour arriving here is therefore still linear HDR at the original scale.

[[vk::binding(0, 1)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] RWTexture2D<float4> inColor;

struct PushConstants {
    uint tonemap; /**< 0 none, 1 ACES - mirrors the trace kernel's dial */
    float exposure;
};
[[vk::push_constant]] PushConstants pushConstants;

// Duplicated from rayquery.slang; keep the three in step.
float3 acesTonemap(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (x * 2.43 + 0.59) + 0.14));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;
    uint2 pixel = dispatchId.xy;

    float3 mapped = max(inColor[pixel].rgb, 0.0) * pushConstants.exposure;
    if (pushConstants.tonemap == 1) mapped = acesTonemap(mapped);
    outputImage[pixel] = float4(pow(max(mapped, 0.0), 1.0 / 2.2), 1.0);
}
