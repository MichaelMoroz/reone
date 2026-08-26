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

import lib.assemble;

[[vk::binding(0, 1)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] RWTexture2D<float4> inNoiseFree;
[[vk::binding(2, 1)]] RWTexture2D<float4> inDiffFactor;
[[vk::binding(3, 1)]] RWTexture2D<float4> inSpecFactor;
// Sampled, not storage: these three are read at a sub-pixel offset and the
// filtering unit does the interpolation. The sampler clamps, so the offset
// cannot wrap the opposite edge of the frame into the kernel.
[[vk::binding(4, 1)]] Sampler2D sDenoisedDiffuse;
[[vk::binding(5, 1)]] Sampler2D sDenoisedSpecular;
[[vk::binding(6, 1)]] RWTexture2D<float> inViewZ;
[[vk::binding(7, 1)]] RWTexture2D<float4> inRawDiffuse;
[[vk::binding(8, 1)]] RWTexture2D<float4> inRawSpecular;
// Direct diffuse at the primary vertex, as whatever settled it: the raw
// channel, the penumbra blur, or its own NRD denoiser. No unpacking to undo -
// it is added in the same currency as the rest, against the same guide factor.
[[vk::binding(9, 1)]] Sampler2D sDirectDiffuse;

/** Mirrors CompositePushConstants in scenepipeline.cpp. */
struct CompositePushConstants {
    /**
     * Where to read the denoised channels from, in pixels, so that they line up
     * with the guide factors they are multiplied against. Leading the float2
     * keeps the block free of padding.
     */
    float2 denoisedJitter;
    /**
     * A debug channel this pass owns, or 0. The three below exist only after
     * denoising, so the trace kernel cannot show them: it writes what it fed
     * the denoiser, and the question here is what came back.
     */
    uint debugView;
    /**
     * The direct channel arrives denoised only under ShadowFilter::Denoiser.
     * Raw and penumbra-filtered both still carry the trace's own jitter, and
     * offsetting those would displace them by a sub-pixel rather than align
     * them.
     */
    uint directDenoised;
    /**
     * Fog: rgb is the linear fog colour, w the global switch (>0.5 on). The
     * per-pixel blend amount rides noiseFree.a - the trace kernel computed it
     * against the surface's own fog feature - so this only carries what is the
     * same for the whole frame. A float4 rather than float3 + uint so the block
     * stays 32 bytes with no alignment gap to guess about.
     */
    float4 fog;
};

static const uint kDebugDenoisedDiffuse = 17;
static const uint kDebugDenoisedSpecular = 18;
static const uint kDebugFilteredDirect = 19;
[[vk::push_constant]] CompositePushConstants pushConstants;


/**
 * RELAX filters linear radiance and hands it back linear, so there is nothing
 * to decode - only the clamp against the small negatives an edge-stopping
 * filter can produce. This was a branch while REBLUR was selectable, since that
 * one filters in YCoCg and its output had to be converted back.
 */
float3 unpackRadiance(float3 stored) {
    return max(stored, 0.0);
}

/**
 * The texture coordinate a denoised channel is read at: this pixel's centre,
 * displaced by the sub-pixel offset the raster beside it was sampled through.
 */
float2 denoisedUv(int2 pixel, float2 extent) {
    return (float2(pixel) + 0.5 + pushConstants.denoisedJitter) / extent;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;
    int2 pixel = int2(dispatchId.xy);

    const float2 extent = float2(width, height);
    // The debug views want what the denoiser produced, so they read at the
    // pixel centre; only the composite below applies the offset.
    const float2 centreUv = (float2(pixel) + 0.5) / extent;

    const float4 noiseFree = inNoiseFree[pixel];
    float3 color = noiseFree.rgb;
    const bool hasDenoisedSurface = !isTerminatingSurface(inViewZ[pixel]);
    float3 diffuse = hasDenoisedSurface ? sDenoisedDiffuse.SampleLevel(centreUv, 0.0).rgb
                                        : inRawDiffuse[pixel].rgb;
    float3 specular = hasDenoisedSurface ? sDenoisedSpecular.SampleLevel(centreUv, 0.0).rgb
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
        outputImage[pixel] = float4(max(sDirectDiffuse.SampleLevel(centreUv, 0.0).rgb, 0.0), 1.0);
        return;
    default:
        break;
    }
    // Sky, and any path-terminating surface, carries viewZ at kPtInfDistance
    // and has all of its radiance in the noise-free channel already. Nothing
    // denoised is assembled onto it.
    //
    // Beyond NRD's range its outputs are undefined, not zero: they are
    // temporally accumulated, and on sky - motion zero, nothing ever drawn -
    // that is last frame's history reprojected. Adding it is what smeared the
    // sky as soon as a denoiser sat behind the direct channel. The diffuse and
    // specular channels only escaped because the tracer leaves them at zero
    // there, which is luck rather than a guarantee.
    if (!hasDenoisedSurface) {
        outputImage[pixel] = float4(color, 1.0);
        return;
    }
    // NRD's temporal pass suppresses jitter by design - its own documentation
    // calls that essential for upscaling - so the denoised channels settle on
    // the pixel centre while the guide factors here were sampled through the
    // jittered projection. The pair therefore disagree by a fixed fraction of a
    // pixel, and standing still that error freezes: the upscaler is handed a
    // static aliased pattern carrying no sub-pixel information and magnifies it
    // instead of reconstructing it. Measured at 1.46x grid correlation against
    // a 1.00x native floor, worst in shadow, and absent under camera motion -
    // which is only NRD's accumulation shortening, not the defect going away.
    //
    // Reading the denoised channels at the same offset as the raster beside
    // them closes the mismatch. It adds no detail that was never shaded; it
    // restores the per-frame variation the upscaler needs to accumulate.
    //
    // The interpolation is the sampler's. A hand-rolled four-tap could reject
    // neighbours past the denoising range, which the filtering unit cannot -
    // but the offset is under half a pixel, so that bleed reaches one pixel at
    // a silhouette rather than the whole sky the viewZ guard above prevents.
    const float2 jitteredUv = denoisedUv(pixel, extent);
    diffuse = sDenoisedDiffuse.SampleLevel(jitteredUv, 0.0).rgb;
    specular = sDenoisedSpecular.SampleLevel(jitteredUv, 0.0).rgb;
    const float3 direct = sDirectDiffuse
                              .SampleLevel(pushConstants.directDenoised != 0 ? jitteredUv : centreUv,
                                           0.0)
                              .rgb;
    color = assembleRadiance(color, unpackRadiance(diffuse), max(direct, 0.0),
                             unpackRadiance(specular), inDiffFactor[pixel].rgb,
                             inSpecFactor[pixel].rgb);
    // Fog last, over the assembled surface radiance. The blend amount is the
    // trace kernel's, per surface, in noiseFree.a; the colour and the master
    // switch are the frame's. A terminator carries 0 here, so the sky path
    // above never reaches this and never needs excluding.
    if (pushConstants.fog.w > 0.5) {
        color = applyFog(color, pushConstants.fog.rgb, noiseFree.a);
    }
    outputImage[pixel] = float4(color, 1.0);
}
