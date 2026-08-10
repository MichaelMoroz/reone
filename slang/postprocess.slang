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
// Full-screen post-processing, ported from glsl/f_pp_gausblur9, f_pp_gausblur13,
// f_pp_fxaa and f_pp_sharpen.
//
// Every stage here reads one texture and writes one, over a triangle covering
// the target, so they share a vertex stage and differ only in the fragment.
// The OpenGL pipeline runs them as separate framebuffer-to-framebuffer draws
// and so does this; what changes is that a Vulkan pass names its own
// attachments rather than binding a framebuffer.
//
// These are deliberately a faithful port rather than an improvement. The whole
// point of keeping FXAA and sharpen for now is that the two backends can be
// compared numerically, and a better antialiaser would end that.

import uniforms;
// For ptDisplayTransform. The transform is colour maths taking its dials as
// arguments, so the shared post-process pass calls the same one the tracer's
// own passes were written against rather than a second copy of it.
import tracing.brdf;

// Screen-space occlusion and reflections used to sit here as two unwired
// full-screen passes. They are wired now, and neither is a post-process:
// occlusion is a term inside the PBR resolve, where the depth and normals it
// needs have already been read, and reflections are a compute dispatch over the
// resolved image. Both live in pbr_resolve.slang, which is where the surfaces
// they modify are shaded. Nothing here referenced them.

// Sampled with an explicit level throughout. These passes read images that
// have no mip chain, so level zero is what OpenGL's texture() resolves to
// anyway - and Slang's implicit-LOD Sample() emits the MinLod capability,
// which needs a device feature that nothing else here wants.
[[vk::binding(0, 1)]] Sampler2D sMainTex;

struct Varyings {
    float4 position : SV_Position;
    noperspective float2 uv : TEXCOORD0;
};

/**
 * The dials of the one display transform, three words wide because that is
 * what every graphics pipeline layout here declares.
 */
struct PostProcessPushConstants {
    /**
     * 0 encode only, 1 encode with the creative grade.
     *
     * The encode is never optional - the scene arrives linear in every mode and
     * something has to put it into display space. What this word gates is the
     * grade: the exposure and the tone curve. Off, the frame is still a correct
     * picture, simply ungraded, which is what makes it a usable diagnostic.
     */
    uint grade;
    float exposure;
    /** 0 none, 1 the Gran Turismo curve; only read when grade is 1. */
    uint tonemap;
};
[[vk::push_constant]] PostProcessPushConstants pushConstants;

/** Mirrors CoveragePushConstants in scenepipeline.cpp. */
struct PrimaryCoveragePushConstants {
    uint skyAvailable;
};
[[vk::push_constant]] PrimaryCoveragePushConstants primaryCoveragePushConstants;

[[vk::binding(21, 1), vk::image_format("r32ui")]] Sampler2D<uint> sGBufTriangleId;
[[vk::binding(22, 1)]] Sampler2D sCoverageTex;
static const uint kNoTriangle = 0xffffffffu;

/**
 * A single triangle covering the target.
 *
 * Larger than the screen and clipped, which beats two triangles: no seam along
 * the diagonal, and one fewer vertex.
 */
[shader("vertex")]
Varyings postVertex(uint vertexId: SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Varyings output;
    output.position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

float rgbToLuma(float3 rgb) {
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

/**
 * The last pass of every mode, and the only one that decides what display
 * colour is.
 *
 * Every mode's scene chain now stops at linear scene-referred colour, so this
 * pass is the encode rather than a decoration: nothing before it is in display
 * space and nothing after it is not. There is no pass-through branch, because
 * a pass-through would present linear radiance as if it were already encoded -
 * which is exactly the bug that made turning post off look like MORE
 * processing rather than less. One shader with dials rather than one per mode,
 * because two implementations of a transform drift.
 */
[shader("fragment")]
float4 postProcessFragment(Varyings input) : SV_Target {
    float4 texel = sMainTex.SampleLevel(input.uv, 0.0);
    // Ungraded means unit exposure and no curve, not an unencoded image.
    const bool graded = pushConstants.grade != 0;
    const float exposure = graded ? pushConstants.exposure : 1.0;
    const uint tonemap = graded ? pushConstants.tonemap : 0;
    return float4(ptDisplayTransform(max(texel.rgb, 0.0), exposure, tonemap),
                  texel.a);
}

/**
 * The tracer has no primary alpha, so a GUI scene establishes the opaque and
 * sky contribution before its forward transparent pass starts accumulating.
 */
[shader("fragment")]
float4 primaryCoverageFragment(Varyings input) : SV_Target {
    float4 texel = sMainTex.SampleLevel(input.uv, 0.0);
    uint width, height;
    sGBufTriangleId.GetDimensions(width, height);
    int2 pixel = min(int2(input.uv * float2(width, height)), int2(width - 1, height - 1));
    bool covered = sGBufTriangleId.Load(int3(pixel, 0)).r != kNoTriangle ||
                   primaryCoveragePushConstants.skyAvailable != 0;
    return float4(texel.rgb, covered ? 1.0 : 0.0);
}

/**
 * The GUI compositor uses alpha as coverage. The source is the image directly
 * after transparent compositing; when FSR has run it remains at render size,
 * because FSR writes its output alpha as one.
 */
[shader("fragment")]
float4 coverageFragment(Varyings input) : SV_Target {
    float4 texel = sMainTex.SampleLevel(input.uv, 0.0);
    return float4(texel.rgb, sCoverageTex.SampleLevel(input.uv, 0.0).a);
}

/** Convert renderer-owned targets into the forms used by the target viewer. */
[shader("fragment")]
float4 debugTextureFragment(Varyings input) : SV_Target {
    float4 texel = sMainTex.SampleLevel(input.uv, 0.0);
    int mode = int(screenEffectUniforms.ssaoSampleRadius);
    float scale = screenEffectUniforms.ssrBias;
    float3 color;
    if (mode == 1) {
        float ndc = 2.0 * texel.r - 1.0;
        float viewZ = 2.0 * screenEffectUniforms.clipNear * screenEffectUniforms.clipFar /
            (screenEffectUniforms.clipFar + screenEffectUniforms.clipNear -
             ndc * (screenEffectUniforms.clipFar - screenEffectUniforms.clipNear));
        color = scale * viewZ.xxx / screenEffectUniforms.clipFar;
    } else if (mode == 2) {
        color = normalize(2.0 * texel.rgb - 1.0) * 0.5 + 0.5;
    } else if (mode == 3) {
        color = float3(0.5 + scale * texel.rg, 0.5);
    } else if (mode == 4) {
        float magnitude = clamp(scale * length(texel.rg), 0.0, 1.0);
        float hue = (atan2(texel.g, texel.r) + 3.14159265359) / 6.28318530718;
        float3 k = fmod(float3(5.0, 3.0, 1.0) + 6.0 * hue, 6.0);
        color = magnitude * (1.0 - max(min(min(k, 4.0 - k), 1.0), 0.0));
    } else {
        color = scale * texel.rgb;
    }
    return float4(color, 1.0);
}

/** An unsharp mask: a five-tap cross, centre weighted up and neighbours down. */
[shader("fragment")]
float4 sharpenFragment(Varyings input) : SV_Target {
    float amount = screenEffectUniforms.sharpenAmount;
    float neighbor = amount * -1.0;
    float center = amount * 4.0 + 1.0;

    float4 rgbaM = sMainTex.SampleLevel(input.uv, 0.0);
    float3 color =
        center * rgbaM.rgb +
        neighbor * sMainTex.SampleLevel(input.uv, 0.0, int2(0, 1)).rgb +
        neighbor * sMainTex.SampleLevel(input.uv, 0.0, int2(-1, 0)).rgb +
        neighbor * sMainTex.SampleLevel(input.uv, 0.0, int2(1, 0)).rgb +
        neighbor * sMainTex.SampleLevel(input.uv, 0.0, int2(0, -1)).rgb;
    return float4(color, rgbaM.a);
}

static const float kSubpix = 0.75;
static const float kEdgeThreshold = 0.166;
static const float kEdgeThresholdMin = 0.0833;

/**
 * How far the edge search steps on each iteration past the first.
 *
 * The original is a preprocessor ladder of nested ifs, one per step, cut off by
 * FXAA_PS. At the quality level this ships with, FXAA_PS 5, exactly these four
 * lengths are compiled in - so a loop over them is the same search, and one can
 * actually read it.
 */
static const float kSearchSteps[4] = {1.5, 2.0, 4.0, 12.0};

/**
 * Runs AFTER the display transform, unlike the temporal resolve that shares its
 * slot. Every threshold below is a luma difference measured against constants
 * (kEdgeThreshold, kEdgeThresholdMin) that were tuned for display-referred
 * colour: on linear input the same scene contrast collapses in the highlights,
 * so bright edges fall under the early-out and are never anti-aliased at all.
 * See ScenePipeline::antiAliasingPass for the rule this follows.
 */
[shader("fragment")]
float4 fxaaFragment(Varyings input) : SV_Target {
    float2 rcp = screenEffectUniforms.screenResolutionRcp;
    float2 posM = input.uv;

    float4 rgbaM = sMainTex.SampleLevel(posM, 0.0);
    float lumaM = rgbToLuma(rgbaM.rgb);
    float lumaS = rgbToLuma(sMainTex.SampleLevel(posM, 0.0, int2(0, 1)).rgb);
    float lumaE = rgbToLuma(sMainTex.SampleLevel(posM, 0.0, int2(1, 0)).rgb);
    float lumaN = rgbToLuma(sMainTex.SampleLevel(posM, 0.0, int2(0, -1)).rgb);
    float lumaW = rgbToLuma(sMainTex.SampleLevel(posM, 0.0, int2(-1, 0)).rgb);

    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    // Flat enough that no edge could run through it. Most of the screen leaves
    // here, which is what makes FXAA cheap.
    if (range < max(kEdgeThresholdMin, rangeMax * kEdgeThreshold)) {
        return rgbaM;
    }

    float lumaNW = rgbToLuma(sMainTex.SampleLevel(posM, 0.0, int2(-1, -1)).rgb);
    float lumaSE = rgbToLuma(sMainTex.SampleLevel(posM, 0.0, int2(1, 1)).rgb);
    float lumaNE = rgbToLuma(sMainTex.SampleLevel(posM, 0.0, int2(1, -1)).rgb);
    float lumaSW = rgbToLuma(sMainTex.SampleLevel(posM, 0.0, int2(-1, 1)).rgb);

    float lumaNS = lumaN + lumaS;
    float lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = (-2.0 * lumaM) + lumaNS;
    float edgeVert1 = (-2.0 * lumaM) + lumaWE;

    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = (-2.0 * lumaE) + lumaNESE;
    float edgeVert2 = (-2.0 * lumaN) + lumaNWNE;

    float lumaNWSW = lumaNW + lumaSW;
    float lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = (abs(edgeHorz1) * 2.0) + abs(edgeHorz2);
    float edgeVert4 = (abs(edgeVert1) * 2.0) + abs(edgeVert2);
    float edgeHorz3 = (-2.0 * lumaW) + lumaNWSW;
    float edgeVert3 = (-2.0 * lumaS) + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4;
    float edgeVert = abs(edgeVert3) + edgeVert4;

    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcp.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;

    // Along a horizontal span the two sides are north and south; along a
    // vertical one they are west and east, so the pair is renamed rather than
    // the rest of the maths being written twice.
    if (!horzSpan) {
        lumaN = lumaW;
        lumaS = lumaE;
    } else {
        lengthSign = rcp.y;
    }
    float subpixB = (subpixA * (1.0 / 12.0)) - lumaM;

    float gradientN = lumaN - lumaM;
    float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    float lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) {
        lengthSign = -lengthSign;
    }
    float subpixC = clamp(abs(subpixB) * subpixRcpRange, 0.0, 1.0);

    float2 posB = posM;
    float2 offNP;
    offNP.x = (!horzSpan) ? 0.0 : rcp.x;
    offNP.y = (horzSpan) ? 0.0 : rcp.y;
    if (!horzSpan) {
        posB.x += lengthSign * 0.5;
    } else {
        posB.y += lengthSign * 0.5;
    }

    float2 posN = posB - offNP;
    float2 posP = posB + offNP;
    float subpixD = ((-2.0) * subpixC) + 3.0;
    float lumaEndN = rgbToLuma(sMainTex.SampleLevel(posN, 0.0).rgb);
    float subpixE = subpixC * subpixC;
    float lumaEndP = rgbToLuma(sMainTex.SampleLevel(posP, 0.0).rgb);

    if (!pairN) {
        lumaNN = lumaSS;
    }
    float gradientScaled = gradient * 1.0 / 4.0;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;

    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;

    // Walk outwards along the edge until both ends have found where the
    // contrast stops. The first step is taken before the loop, matching the
    // original, which advances once and only then begins re-sampling.
    if (!doneN) {
        posN -= offNP * kSearchSteps[0];
    }
    if (!doneP) {
        posP += offNP * kSearchSteps[0];
    }
    for (int i = 1; i < 4; ++i) {
        if (doneN && doneP) {
            break;
        }
        if (!doneN) {
            lumaEndN = rgbToLuma(sMainTex.SampleLevel(posN, 0.0).rgb) - lumaNN * 0.5;
        }
        if (!doneP) {
            lumaEndP = rgbToLuma(sMainTex.SampleLevel(posP, 0.0).rgb) - lumaNN * 0.5;
        }
        doneN = abs(lumaEndN) >= gradientScaled;
        doneP = abs(lumaEndP) >= gradientScaled;
        if (!doneN) {
            posN -= offNP * kSearchSteps[i];
        }
        if (!doneP) {
            posP += offNP * kSearchSteps[i];
        }
    }

    // Along the axis the search actually walked, which offNP already chose: x
    // for a horizontal span, y for a vertical one. Measuring the other axis
    // gives a span length of zero, and the pixel offset derived from it is
    // then meaningless - the edges are still found, but every one of them is
    // blended by the wrong amount.
    float dstN = horzSpan ? (posM.x - posN.x) : (posM.y - posN.y);
    float dstP = horzSpan ? (posP.x - posM.x) : (posP.y - posM.y);

    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    float spanLengthRcp = 1.0 / spanLength;

    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = (dst * (-spanLengthRcp)) + 0.5;
    float subpixH = subpixG * kSubpix;

    // Two candidate shifts: one from the span the search measured, one from how
    // much this pixel differs from its neighbourhood. The larger wins, so a
    // pixel with no usable span still gets subpixel treatment.
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) {
        posM.x += pixelOffsetSubpix * lengthSign;
    } else {
        posM.y += pixelOffsetSubpix * lengthSign;
    }
    return float4(sMainTex.SampleLevel(posM, 0.0).rgb, rgbaM.a);
}
