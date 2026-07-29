/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The post-denoise assembly: the same contract the trace kernel applies when
// the denoiser is off, evaluated from NRD's outputs instead of the raw
// channels - final = noiseFree + denoisedDiffuse * albedo + denoisedSpecular
// - followed by the one display transform. Runs instead of trusting the trace
// kernel's own final write, which it overwrites.

[[vk::binding(0, 0)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 0)]] RWTexture2D<float4> inNoiseFree;
[[vk::binding(2, 0)]] RWTexture2D<float4> inAlbedo;
[[vk::binding(3, 0)]] RWTexture2D<float4> inDenoisedDiffuse;
[[vk::binding(4, 0)]] RWTexture2D<float4> inDenoisedSpecular;

struct PushConstants {
    uint tonemap; /**< 0 none, 1 ACES - mirrors the trace kernel's dial */
    float exposure;
};
[[vk::push_constant]] PushConstants pushConstants;

// Duplicated from rayquery.slang rather than shared: the trace kernel's copy
// reads its dial out of packed traceFlags, and a two-function include was not
// worth a slang/lib module. Keep the two in step.
float3 acesTonemap(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (x * 2.43 + 0.59) + 0.14));
}

float3 displayTransform(float3 linearColor) {
    float3 mapped = linearColor * pushConstants.exposure;
    if (pushConstants.tonemap == 1) mapped = acesTonemap(mapped);
    return pow(max(mapped, 0.0), 1.0 / 2.2);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;
    uint2 pixel = dispatchId.xy;
    // REBLUR filtered in YCoCg; back to RGB before remodulation. Mirrors
    // _NRD_YCoCgToLinear in NRD.hlsli.
    float3 diffuse = inDenoisedDiffuse[pixel].rgb;
    float t = diffuse.x - diffuse.z * 0.5;
    float g = diffuse.z + t;
    float b = t - diffuse.y * 0.5;
    diffuse = max(float3(b + diffuse.y, g, b), 0.0);
    float3 specular = inDenoisedSpecular[pixel].rgb;
    t = specular.x - specular.z * 0.5;
    g = specular.z + t;
    b = t - specular.y * 0.5;
    specular = max(float3(b + specular.y, g, b), 0.0);
    float3 color = inNoiseFree[pixel].rgb + diffuse * inAlbedo[pixel].rgb + specular;
    outputImage[pixel] = float4(displayTransform(color), 1.0);
}
