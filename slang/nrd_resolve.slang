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

// The post-denoise assembly uses the guide factors that accompanied the
// demodulated radiance through NRD, overwriting the trace kernel's own write.
//
// Not everything it assembles went through NRD. Direct diffuse at the primary
// vertex arrives on a channel of its own, untouched: it carries the shadow the
// viewer looks straight at, and a spatial filter wide enough to settle bounce
// noise is wide enough to soften a contact edge. It is stratified at the source
// instead, and whatever noise survives is left to the temporal resolve.
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
// Direct diffuse at the primary vertex. It never entered NRD, so there is no
// denoised counterpart to fall back from and no unpacking to undo - it is added
// here in the same currency as the rest, against the same guide factor.
[[vk::binding(9, 1)]] RWTexture2D<float4> inDirectDiffuse;

/** Mirrors NrdResolvePushConstants in tracingpipeline.cpp. */
struct NrdResolvePushConstants {
    /** Non-zero when RELAX ran, whose channels are already linear. */
    uint relax;
    /**
     * A debug channel this pass owns, or 0. The three below exist only after
     * denoising, so the trace kernel cannot show them: it writes what it fed
     * the denoiser, and the question here is what came back.
     */
    uint debugView;
};

static const uint kDebugDenoisedDiffuse = 17;
static const uint kDebugDenoisedSpecular = 18;
static const uint kDebugFilteredDirect = 19;
[[vk::push_constant]] NrdResolvePushConstants pushConstants;

/** Denoised channels are undefined beyond NRD's denoising range. */
static const float kDenoisedRange = 1.0e5;

/** _NRD_YCoCgToLinear from NRD.hlsli. REBLUR filters in YCoCg; RELAX does not. */
float3 yCoCgToLinear(float3 color) {
    float t = color.x - color.z;
    float3 r;
    r.y = color.x + color.z;
    r.x = t + color.y;
    r.z = t - color.y;
    return max(r, 0.0);
}

float3 unpackRadiance(float3 packed) {
    return pushConstants.relax != 0u ? max(packed, 0.0) : yCoCgToLinear(packed);
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
    // Demodulated, deliberately: these show what the denoiser produced, not
    // what it looks like once the material is put back, because a channel that
    // has been multiplied by an albedo no longer shows its own noise.
    switch (pushConstants.debugView) {
    case kDebugDenoisedDiffuse:
        outputImage[pixel] = float4(unpackRadiance(diffuse), 1.0);
        return;
    case kDebugDenoisedSpecular:
        outputImage[pixel] = float4(unpackRadiance(specular), 1.0);
        return;
    case kDebugFilteredDirect:
        outputImage[pixel] = float4(max(inDirectDiffuse[pixel].rgb, 0.0), 1.0);
        return;
    default:
        break;
    }
    color += (unpackRadiance(diffuse) + max(inDirectDiffuse[pixel].rgb, 0.0)) *
                 inDiffFactor[pixel].rgb +
             unpackRadiance(specular) * inSpecFactor[pixel].rgb;
    outputImage[pixel] = float4(color, 1.0);
}
