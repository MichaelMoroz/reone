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

// Screen-space reflections, over the assembled image.
//
// A dispatch of its own, after the composite: a reflection lands on some OTHER
// pixel, and a kernel that read the image it is writing would be reading
// whichever neighbouring workgroups happened to have run. It reflects the lit
// image rather than a G-buffer product - a hit therefore carries the shadows,
// the lightmap and the sky that pixel actually shows.
//
// Composited as a lerp weighted by the authored mirror share, never as an add.
// The channels pass has already put the authored cube into these pixels at
// exactly (1 - diffuse alpha); replacing that share where the screen holds a
// real hit is a substitution, while adding would light the surface twice.
//
// This is PBR's screen-space stand-in for traced specular; making it a provider
// the tracer's contract carries end-to-end is RAS-037.

import uniforms;
import lib.gbuf;

[[vk::binding(0, 1)]] Sampler2D sSceneColor;
[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(0, 3)]] RWTexture2D<float4> outImage;

static const float kSsrMaxDistance = 100.0;
static const float kEdgeFadeStart = 0.8;

float3 reconstructViewPos(float2 uv) {
    return gbufViewPosition(uv, sGBufDepth.SampleLevel(uv, 0.0).r, globalUniforms.projectionInv);
}

float distanceSquared(float2 a, float2 b) { float2 d = a - b; return dot(d, d); }
void swapIfGreater(inout float a, inout float b) { if (a > b) { float t = a; a = b; b = t; } }

/**
 * The DDA march of Morgan McGuire's efficient screen-space ray trace, in view
 * space, stepping in whichever screen axis the ray covers faster.
 */
bool traceScreenSpaceRay(float3 rayOrigin, float3 rayDirection, float jitter,
                         out float2 hitUV, out float numSteps) {
    hitUV = float2(0.0);
    numSteps = 0.0;
    float rayLength =
        rayOrigin.z + rayDirection.z * kSsrMaxDistance > -screenEffectUniforms.clipNear
            ? (-screenEffectUniforms.clipNear - rayOrigin.z) / rayDirection.z
            : kSsrMaxDistance;
    float3 rayEnd = rayOrigin + rayDirection * rayLength;
    float4 h0 = mul(globalUniforms.projection, float4(rayOrigin, 1.0));
    float k0 = 1.0 / h0.w;
    float3 q0 = rayOrigin * k0;
    float2 p0 = float2(h0.x * k0 * 0.5 + 0.5, 0.5 - h0.y * k0 * 0.5) *
                screenEffectUniforms.screenResolution;
    float4 h1 = mul(globalUniforms.projection, float4(rayEnd, 1.0));
    float k1 = 1.0 / h1.w;
    float3 q1 = rayEnd * k1;
    float2 p1 = float2(h1.x * k1 * 0.5 + 0.5, 0.5 - h1.y * k1 * 0.5) *
                screenEffectUniforms.screenResolution;
    p1 += distanceSquared(p0, p1) < 0.0001 ? float2(0.01) : float2(0.0);
    float2 delta = p1 - p0;
    bool permute = abs(delta.x) < abs(delta.y);
    if (permute) { delta = delta.yx; p0 = p0.yx; p1 = p1.yx; }
    float stepDir = sign(delta.x);
    float invdx = stepDir / delta.x;
    float3 dQ = (q1 - q0) * invdx;
    float dk = (k1 - k0) * invdx;
    float2 dP = float2(stepDir, delta.y * invdx);
    float stride = screenEffectUniforms.ssrPixelStride;
    dP *= stride; dQ *= stride; dk *= stride;
    p0 += (1.0 + jitter) * dP;
    q0 += (1.0 + jitter) * dQ;
    k0 += (1.0 + jitter) * dk;
    float2 p = p0;
    float3 q = q0;
    float k = k0;
    float end = p1.x * stepDir;
    float previousZMax = rayOrigin.z;
    for (float i = 0.0; i < screenEffectUniforms.ssrMaxSteps && p.x * stepDir <= end; i += 1.0) {
        float2 sceneUV = (permute ? p.yx : p) * screenEffectUniforms.screenResolutionRcp;
        float rayZMin = previousZMax;
        float rayZMax = (dQ.z * 0.5 + q.z) / (dk * 0.5 + k);
        previousZMax = rayZMax;
        swapIfGreater(rayZMin, rayZMax);
        float sceneZ = reconstructViewPos(sceneUV).z;
        if (rayZMin <= sceneZ && rayZMax >= sceneZ - screenEffectUniforms.ssrBias) {
            hitUV = sceneUV;
            numSteps = i;
            return true;
        }
        p += dP; q += dQ; k += dk;
    }
    return false;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ssrMain(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) {
        return;
    }
    int2 pixel = int2(dispatchId.xy);
    float2 uv = (float2(pixel) + 0.5) / float2(width, height);
    // Every pixel is written: this dispatch produces the whole image, not a
    // reflection layer somebody else composites.
    float4 scene = sSceneColor.SampleLevel(uv, 0.0);

    // Nothing was drawn here, so there is no surface to reflect off. Far depth
    // is the sky, which the composite has already written into this pixel.
    float depth = sGBufDepth.SampleLevel(uv, 0.0).r;
    float4 mainTexSample = sGBufDiffuse.SampleLevel(uv, 0.0);
    float mirror = 1.0 - mainTexSample.a;
    if (depth >= 1.0 || mirror <= 0.0) {
        outImage[pixel] = scene;
        return;
    }

    float3 position = reconstructViewPos(uv);
    float3 eyeNormal = normalize(2.0 * sGBufEyeNormal.SampleLevel(uv, 0.0).xyz - 1.0);
    float3 reflection = reflect(normalize(position), eyeNormal);
    // Half a stride on a checkerboard, so that neighbouring rays do not all
    // test the same depths and step the same misses.
    float jitter = float((pixel.x + pixel.y) & 1) * 0.5;

    float2 hitUV;
    float numSteps;
    float3 reflectionColor = float3(0.0);
    float strength = 0.0;
    if (traceScreenSpaceRay(position, reflection, jitter, hitUV, numSteps)) {
        float2 hitNDC = hitUV * 2.0 - 1.0;
        float maxDim = min(1.0, max(abs(hitNDC.x), abs(hitNDC.y)));
        reflectionColor = sSceneColor.SampleLevel(hitUV, 0.0).rgb;
        // Three confidences, all of them about how much the screen actually
        // knows: a ray leaving toward the camera has nothing in front of it, a
        // ray that marched to its step limit never converged, and a hit near
        // the border reflects a surface that leaves the screen next frame.
        strength = 1.0 - clamp(reflection.z, 0.0, 1.0);
        strength *= 1.0 - numSteps / screenEffectUniforms.ssrMaxSteps;
        strength *= 1.0 - max(0.0, (maxDim - kEdgeFadeStart) / (1.0 - kEdgeFadeStart));
    }
    outImage[pixel] = float4(lerp(scene.rgb, reflectionColor, mirror * strength), scene.a);
}
