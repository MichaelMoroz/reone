/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// The post-denoise assembly: final = TAA(noiseFree) + denoisedDiffuse *
// albedo + denoisedSpecular, then the one display transform, overwriting the
// trace kernel's own write.
//
// Two jobs beyond the sum. The denoised channels only exist inside NRD's
// denoising range - REBLUR never writes its outputs at sky depth, so adding
// them there splashed uninitialized pool memory across the sky - hence the
// viewZ guard. And the noise-free channel (transparency layers, additive
// glows, emission, the sky itself) passes through no denoiser, which under
// TAA jitter made grass edges and the sky shimmer; it gets its own history:
// reprojected through the same motion vectors and matrices NRD uses,
// neighborhood-clamped against the current frame to bound ghosting.

import uniforms;

[[vk::binding(0, 1)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] RWTexture2D<float4> inNoiseFree;
[[vk::binding(2, 1)]] RWTexture2D<float4> inAlbedo;
[[vk::binding(3, 1)]] RWTexture2D<float4> inDenoisedDiffuse;
[[vk::binding(4, 1)]] RWTexture2D<float4> inDenoisedSpecular;
[[vk::binding(5, 1)]] RWTexture2D<float> inViewZ;
[[vk::binding(6, 1)]] RWTexture2D<float4> inMotion;
[[vk::binding(7, 1)]] RWTexture2D<float4> historyIn;
[[vk::binding(8, 1)]] RWTexture2D<float4> historyOut;

struct PushConstants {
    uint tonemap;       /**< 0 none, 1 ACES - mirrors the trace kernel's dial */
    float exposure;
    float historyBlend; /**< 0 on the first frame and across teleports */
    /**
     * This frame's sub-pixel jitter in pixels, UV-down y. The trace pass
     * renders on the jittered grid; the composite is the resolve that puts
     * the image back on the stable display grid - without the compensation
     * the whole frame vibrates by the jitter sequence.
     */
    float jitterX;
    float jitterY;
};
[[vk::push_constant]] PushConstants pushConstants;

/** Manual bilinear over a storage image at a fractional pixel position. */
float4 bilinear(RWTexture2D<float4> image, float2 position, int2 size) {
    float2 base = position - 0.5;
    int2 p0 = int2(floor(base));
    float2 f = base - float2(p0);
    int2 p1 = clamp(p0 + int2(1, 0), int2(0), size - 1);
    int2 p2 = clamp(p0 + int2(0, 1), int2(0), size - 1);
    int2 p3 = clamp(p0 + int2(1, 1), int2(0), size - 1);
    p0 = clamp(p0, int2(0), size - 1);
    return lerp(lerp(image[p0], image[p1], f.x), lerp(image[p2], image[p3], f.x), f.y);
}

/** Denoised channels are undefined beyond NRD's denoising range. */
static const float kDenoisedRange = 1.0e5;

// Duplicated from rayquery.slang; keep the two in step.
float3 acesTonemap(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (x * 2.43 + 0.59) + 0.14));
}

float3 displayTransform(float3 linearColor) {
    float3 mapped = linearColor * pushConstants.exposure;
    if (pushConstants.tonemap == 1) mapped = acesTonemap(mapped);
    return pow(max(mapped, 0.0), 1.0 / 2.2);
}

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
    uint2 pixel = dispatchId.xy;

    // The resolve: this display pixel's content was rendered at the jittered
    // sample position. All fetches below use it; the write stays on the
    // stable grid.
    int2 size = int2(width, height);
    float2 samplePos = float2(pixel) + 0.5 + float2(pushConstants.jitterX, pushConstants.jitterY);
    int2 samplePixel = clamp(int2(samplePos), int2(0), size - 1);
    float viewZ = inViewZ[samplePixel];
    float3 noiseFree = bilinear(inNoiseFree, samplePos, size).rgb;

    // Reproject: the pixel's world position reconstructed from viewZ along
    // the (jittered, matching the rays) camera ray, moved by the world-space
    // motion vector, projected by the unjittered previous-frame matrix.
    // A sky pixel reconstructs a point far along the ray, which reprojects
    // as a pure direction - exactly what a shifting camera needs.
    float2 uv = samplePos / float2(width, height);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 nearH = mul(globalUniforms.projectionInv, float4(ndc, 0.0, 1.0));
    float4 farH = mul(globalUniforms.projectionInv, float4(ndc, 1.0, 1.0));
    float3 nearView = nearH.xyz / nearH.w;
    float3 farView = farH.xyz / farH.w;
    float3 dirView = farView - nearView;
    float t = (viewZ - nearView.z) / (abs(dirView.z) > 1e-9 ? dirView.z : 1e-9);
    float3 viewPos = nearView + dirView * t;
    float3 worldPos = mul(globalUniforms.viewInv, float4(viewPos, 1.0)).xyz;
    float3 prevWorldPos = worldPos + inMotion[samplePixel].xyz;
    float4 prevClip = mul(globalUniforms.prevViewProjection, float4(prevWorldPos, 1.0));
    float3 history = noiseFree;
    bool historyValid = pushConstants.historyBlend > 0.0 && prevClip.w > 1e-4;
    if (historyValid) {
        float2 prevNdc = prevClip.xy / prevClip.w;
        float2 prevUv = float2(prevNdc.x, -prevNdc.y) * 0.5 + 0.5;
        if (all(prevUv >= 0.0) && all(prevUv < 1.0)) {
            int2 prevPixel = int2(prevUv * float2(width, height));
            history = historyIn[prevPixel].rgb;
            // Clamp to the current 3x3 neighborhood: bounds ghosting from
            // stale history without tracking validity per pixel.
            float3 boxMin = noiseFree;
            float3 boxMax = noiseFree;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int2 q = clamp(samplePixel + int2(dx, dy), int2(0), size - 1);
                    float3 c = inNoiseFree[q].rgb;
                    boxMin = min(boxMin, c);
                    boxMax = max(boxMax, c);
                }
            }
            history = clamp(history, boxMin, boxMax);
        } else {
            historyValid = false;
        }
    }
    float3 stabilized = lerp(noiseFree, history, historyValid ? pushConstants.historyBlend : 0.0);
    historyOut[pixel] = float4(stabilized, 1.0);

    float3 color = stabilized;
    if (abs(viewZ) < kDenoisedRange) {
        color += yCoCgToLinear(bilinear(inDenoisedDiffuse, samplePos, size).rgb) *
                     bilinear(inAlbedo, samplePos, size).rgb +
                 yCoCgToLinear(bilinear(inDenoisedSpecular, samplePos, size).rgb);
    }
    outputImage[pixel] = float4(displayTransform(color), 1.0);
}
