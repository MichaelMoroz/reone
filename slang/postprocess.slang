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

// Sampled with an explicit level throughout. These passes read images that
// have no mip chain, so level zero is what OpenGL's texture() resolves to
// anyway - and Slang's implicit-LOD Sample() emits the MinLod capability,
// which needs a device feature that nothing else here wants.
[[vk::binding(0, 1)]] Sampler2D sMainTex;

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

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
 * Nine-tap Gaussian, separated: the pipeline runs it twice, once per axis, with
 * blurDirection saying which. The taps are the usual linear-sampling trick -
 * five texture reads standing in for nine, by landing between texels and
 * letting the bilinear filter do the pairwise sum.
 */
[shader("fragment")]
float4 gausBlur9Fragment(Varyings input) : SV_Target {
    float2 uv = input.uv;
    float2 direction = screenEffectUniforms.blurDirection;
    float2 rcp = screenEffectUniforms.screenResolutionRcp;

    float4 color = sMainTex.SampleLevel(uv, 0.0);
    color.rgb *= 0.2270270270;

    float2 off1 = 1.3846153846 * direction;
    float2 off2 = 3.2307692308 * direction;
    color.rgb += sMainTex.SampleLevel(uv + off1 * rcp, 0.0).rgb * 0.3162162162;
    color.rgb += sMainTex.SampleLevel(uv - off1 * rcp, 0.0).rgb * 0.3162162162;
    color.rgb += sMainTex.SampleLevel(uv + off2 * rcp, 0.0).rgb * 0.0702702703;
    color.rgb += sMainTex.SampleLevel(uv - off2 * rcp, 0.0).rgb * 0.0702702703;
    return color;
}

/**
 * Thirteen-tap Gaussian, same separated scheme with a wider kernel.
 *
 * Unlike the nine-tap, this one filters alpha along with colour rather than
 * carrying the centre tap's alpha through. That difference is in the originals
 * and is preserved here; the two are used on different targets.
 */
[shader("fragment")]
float4 gausBlur13Fragment(Varyings input) : SV_Target {
    float2 uv = input.uv;
    float2 direction = screenEffectUniforms.blurDirection;
    float2 rcp = screenEffectUniforms.screenResolutionRcp;

    float2 off1 = 1.411764705882353 * direction;
    float2 off2 = 3.2941176470588234 * direction;
    float2 off3 = 5.176470588235294 * direction;

    float4 color = sMainTex.SampleLevel(uv, 0.0) * 0.1964825501511404;
    color += sMainTex.SampleLevel(uv + off1 * rcp, 0.0) * 0.2969069646728344;
    color += sMainTex.SampleLevel(uv - off1 * rcp, 0.0) * 0.2969069646728344;
    color += sMainTex.SampleLevel(uv + off2 * rcp, 0.0) * 0.09447039785044732;
    color += sMainTex.SampleLevel(uv - off2 * rcp, 0.0) * 0.09447039785044732;
    color += sMainTex.SampleLevel(uv + off3 * rcp, 0.0) * 0.010381362401148057;
    color += sMainTex.SampleLevel(uv - off3 * rcp, 0.0) * 0.010381362401148057;
    return color;
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
