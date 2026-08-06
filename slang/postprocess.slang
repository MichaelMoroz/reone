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

// --- Screen-space occlusion and reflections: full-screen passes like the
// rest of this module, sharing its varyings and main-texture binding.
// Unwired pending RAS-019 and RAS-020 ---
// Screen-space approximations of what the tracer computes properly: ambient
// occlusion and reflections. Neither is wired into a pipeline - RAS-019 and
// RAS-020 decide whether they survive at all - so they share one module
// rather than two the registry compiles for nothing. The reflection half
// reuses the occlusion half's varyings, fullscreen vertex stage and
// view-position reconstruction, which were identical.


[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(6, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(12, 1)]] Sampler2D sNoise;

static const float kNoiseScale = 1.0 / 4.0;
static const float kSsaoMaxDistance = 500.0;


[shader("vertex")]
Varyings ssaoVertex(uint vertexId: SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Varyings output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

// The G-buffer's viewport is y-flipped and depth is already 0..1.
float3 reconstructViewPos(float2 uv) {
    float depth = sGBufDepth.SampleLevel(uv, 0.0).r;
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, depth);
    float4 viewPos = mul(globalUniforms.projectionInv, float4(ndc, 1.0));
    return viewPos.xyz / viewPos.w;
}

[shader("fragment")]
float4 ssaoFragment(Varyings input) : SV_Target {
    float2 uv = input.uv;
    float3 pos = reconstructViewPos(uv);
    float3 normal = normalize(2.0 * sGBufEyeNormal.SampleLevel(uv, 0.0).xyz - 1.0);
    float3 random = float3(sNoise.SampleLevel(uv * screenEffectUniforms.screenResolution * kNoiseScale, 0.0).rg, 0.0);
    float3 tangent = normalize(random - normal * dot(random, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 tbn = float3x3(tangent, bitangent, normal);
    float occlusion = 0.0;
    for (int i = 0; i < kNumSSAOSamples; ++i) {
        // GLSL mat3 takes these basis vectors as columns. Slang lowers this
        // constructor as the same SPIR-V columns, so multiply the sample from
        // the left to obtain tangent*x + bitangent*y + normal*z.
        float3 samplePos = pos + screenEffectUniforms.ssaoSampleRadius * mul(screenEffectUniforms.ssaoSamples[i].xyz, tbn);
        float4 sampleClip = mul(globalUniforms.projection, float4(samplePos, 1.0));
        float2 sampleUV = sampleClip.xy / sampleClip.w;
        sampleUV = float2(sampleUV.x * 0.5 + 0.5, 0.5 - sampleUV.y * 0.5);
        float sceneZ = reconstructViewPos(sampleUV).z;
        float rangeCheck = smoothstep(0.0, 1.0, screenEffectUniforms.ssaoSampleRadius / abs(pos.z - sceneZ));
        occlusion += (sceneZ >= samplePos.z + screenEffectUniforms.ssaoBias ? 1.0 : 0.0) * rangeCheck;
    }
    occlusion = 1.0 - (occlusion / float(kNumSSAOSamples)) * (1.0 - smoothstep(0.0, kSsaoMaxDistance, abs(pos.z)));
    return float4(occlusion.xxx, 1.0);
}

// --- Screen-space reflections ---

[[vk::binding(1, 1)]] Sampler2D sLightmap;

static const float kSsrMaxDistance = 100.0;
static const float kEdgeFadeStart = 0.8;


float distanceSquared(float2 a, float2 b) { float2 d = a - b; return dot(d, d); }
void swapIfGreater(inout float a, inout float b) { if (a > b) { float t = a; a = b; b = t; } }

bool traceScreenSpaceRay(float3 rayOrigin, float3 rayDirection, float jitter, out float2 hitUV, out float3 hitPoint, out float numSteps) {
    hitUV = float2(0.0); hitPoint = float3(0.0); numSteps = 0.0;
    float rayLength = rayOrigin.z + rayDirection.z * kSsrMaxDistance > -screenEffectUniforms.clipNear ? (-screenEffectUniforms.clipNear - rayOrigin.z) / rayDirection.z : kSsrMaxDistance;
    float3 rayEnd = rayOrigin + rayDirection * rayLength;
    float4 h0 = mul(globalUniforms.projection, float4(rayOrigin, 1.0));
    float k0 = 1.0 / h0.w; float3 q0 = rayOrigin * k0;
    float2 p0 = float2(h0.x * k0 * 0.5 + 0.5, 0.5 - h0.y * k0 * 0.5) * screenEffectUniforms.screenResolution;
    float4 h1 = mul(globalUniforms.projection, float4(rayEnd, 1.0));
    float k1 = 1.0 / h1.w; float3 q1 = rayEnd * k1;
    float2 p1 = float2(h1.x * k1 * 0.5 + 0.5, 0.5 - h1.y * k1 * 0.5) * screenEffectUniforms.screenResolution;
    p1 += distanceSquared(p0, p1) < 0.0001 ? float2(0.01) : float2(0.0);
    float2 delta = p1 - p0; bool permute = abs(delta.x) < abs(delta.y);
    if (permute) { delta = delta.yx; p0 = p0.yx; p1 = p1.yx; }
    float stepDir = sign(delta.x); float invdx = stepDir / delta.x;
    float3 dQ = (q1 - q0) * invdx; float dk = (k1 - k0) * invdx; float2 dP = float2(stepDir, delta.y * invdx);
    float stride = screenEffectUniforms.ssrPixelStride;
    dP *= stride; dQ *= stride; dk *= stride;
    p0 += (1.0 + jitter) * dP; q0 += (1.0 + jitter) * dQ; k0 += (1.0 + jitter) * dk;
    float2 p = p0; float3 q = q0; float k = k0; float end = p1.x * stepDir; float previousZMax = rayOrigin.z;
    for (float i = 0.0; i < screenEffectUniforms.ssrMaxSteps && p.x * stepDir <= end; i += 1.0) {
        float2 sceneUV = (permute ? p.yx : p) * screenEffectUniforms.screenResolutionRcp;
        float rayZMin = previousZMax; float rayZMax = (dQ.z * 0.5 + q.z) / (dk * 0.5 + k); previousZMax = rayZMax;
        swapIfGreater(rayZMin, rayZMax);
        float sceneZ = reconstructViewPos(sceneUV).z;
        if (rayZMin <= sceneZ && rayZMax >= sceneZ - screenEffectUniforms.ssrBias) { hitUV = sceneUV; hitPoint = q / k; numSteps = i; return true; }
        p += dP; q += dQ; k += dk;
    }
    return false;
}

[shader("fragment")]
float4 ssrFragment(Varyings input) : SV_Target {
    float4 mainTex = sMainTex.SampleLevel(input.uv, 0.0);
    if (mainTex.a == 1.0) return float4(0.0);
    float3 position = reconstructViewPos(input.uv);
    float3 reflection = reflect(normalize(position), normalize(2.0 * sGBufEyeNormal.SampleLevel(input.uv, 0.0).xyz - 1.0));
    uint2 pixel = uint2(input.position.xy); float jitter = float((pixel.x + pixel.y) & 1) * 0.5;
    float2 hitUV; float3 hitPoint; float numSteps; float3 reflectionColor = float3(0.0); float reflectionStrength = 0.0;
    if (traceScreenSpaceRay(position, reflection, jitter, hitUV, hitPoint, numSteps)) {
        float2 hitNDC = hitUV * 2.0 - 1.0; float maxDim = min(1.0, max(abs(hitNDC.x), abs(hitNDC.y)));
        float4 hitMain = sMainTex.SampleLevel(hitUV, 0.0); float4 hitLightmap = sLightmap.SampleLevel(hitUV, 0.0);
        reflectionColor = hitMain.rgb * hitMain.a * hitLightmap.rgb;
        reflectionStrength = 1.0 - clamp(reflection.z, 0.0, 1.0);
        reflectionStrength *= 1.0 - numSteps / screenEffectUniforms.ssrMaxSteps;
        reflectionStrength *= 1.0 - max(0.0, (maxDim - kEdgeFadeStart) / (1.0 - kEdgeFadeStart));
    }
    return float4(reflectionColor, reflectionStrength);
}

// Sampled with an explicit level throughout. These passes read images that
// have no mip chain, so level zero is what OpenGL's texture() resolves to
// anyway - and Slang's implicit-LOD Sample() emits the MinLod capability,
// which needs a device feature that nothing else here wants.
[[vk::binding(0, 1)]] Sampler2D sMainTex;
// Read by oitBlendFragment alone. The units are the ones TextureUnits names,
// so the pipeline binds them the same way it binds anything else.
[[vk::binding(10, 1)]] Sampler2D sOITAccum;
[[vk::binding(11, 1)]] Sampler2D sOITRevealage;
[[vk::binding(9, 1)]] Sampler2D sHilights;

struct Varyings {
    float4 position : SV_Position;
    noperspective float2 uv : TEXCOORD0;
};

/**
 * The dials of the one display transform, three words wide because that is
 * what every graphics pipeline layout here declares.
 */
struct PostProcessPushConstants {
    /** 0 pass the image through, 1 linear scene colour to display colour. */
    uint transform;
    float exposure;
    /** 0 none, 1 ACES; ignored when the transform is a pass-through. */
    uint tonemap;
};
[[vk::push_constant]] PostProcessPushConstants pushConstants;

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
 * A pass-through is not a no-op that could be skipped: the raster resolves
 * write display-space colour already, so the identity is what keeps their
 * bytes exactly what they were while the traced chain, which stops at linear,
 * is finished here. One shader with a dial rather than one per mode, because
 * two implementations of a transform drift.
 */
[shader("fragment")]
float4 postProcessFragment(Varyings input) : SV_Target {
    float4 texel = sMainTex.SampleLevel(input.uv, 0.0);
    if (pushConstants.transform == 0) {
        return texel;
    }
    return float4(ptDisplayTransform(max(texel.rgb, 0.0), pushConstants.exposure,
                                     pushConstants.tonemap),
                  texel.a);
}

/** The small square blur used to soften half-resolution SSAO. */
[shader("fragment")]
float4 boxBlur4Fragment(Varyings input) : SV_Target {
    float3 color = float3(0.0);
    for (int x = -2; x < 2; ++x) {
        for (int y = -2; y < 2; ++y) {
            color += sMainTex.SampleLevel(input.uv, 0.0, int2(x, y)).rgb;
        }
    }
    return float4(color * (1.0 / 16.0), 1.0);
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

/**
 * Resolve the weighted-blended transparency targets onto the opaque image,
 * ported from glsl/f_oit_blend.glsl.
 *
 * The accumulated colour is a weighted sum, so it is divided by the sum of the
 * weights to get an average, and revealage says how much of the opaque image
 * survives underneath. Both come out of the pass's blend state rather than
 * being computed here; see BlendMode::OIT_Transparent.
 */
[shader("fragment")]
float4 oitBlendFragment(Varyings input) : SV_Target {
    float4 mainTexSample = sMainTex.SampleLevel(input.uv, 0.0);
    float4 accumSample = sOITAccum.SampleLevel(input.uv, 0.0);
    float accumWeight = sOITRevealage.SampleLevel(input.uv, 0.0).r;

    float3 hilights = sHilights.SampleLevel(input.uv, 0.0).rgb;

    float revealage = accumSample.a;
    float alpha = 1.0 - revealage;
    float3 color = alpha * (accumSample.rgb / max(0.0001, accumWeight)) +
                   (1.0 - alpha) * (mainTexSample.rgb + hilights);
    return float4(color, alpha + mainTexSample.a);
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
