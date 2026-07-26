module hashedalpha;

// Hashed alpha testing, as used for foliage and other punch-through surfaces.
// Ported from glsl/i_hash.glsl and glsl/i_hashedalpha.glsl.

// This was frac(1e4 * sin(...) * ...), matching the original GLSL. That is not
// reproducible between backends: sin() is not bit-exact across the GLSL and
// SPIR-V compilation paths, and multiplying by ten thousand before taking the
// fraction turns a difference of one ulp into an unrelated number. The alpha
// test then kept different fragments under each backend, which showed up as
// speckle over every piece of foliage - depth and the sampled alpha agreed,
// only the threshold did not. glsl/i_hash.glsl carries the same integer hash.
uint hashUint(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/** The caller has already floored its input to a cell. */
float hash3(float3 p) {
    int3 c = int3(p);
    uint h = hashUint(uint(c.x) ^ hashUint(uint(c.y) ^ hashUint(uint(c.z))));
    return float(h) * (1.0 / 4294967296.0);
}

/** @return true when the fragment should be discarded */
public bool hashedAlphaTestFails(float alpha, float3 p) {
    float maxDeriv = max(length(ddx(p.xy)), length(ddy(p.xy)));
    float pixScale = 1.0 / maxDeriv;
    float2 pixScales = float2(
        exp2(floor(log2(pixScale))),
        exp2(ceil(log2(pixScale))));
    float2 a = float2(
        hash3(floor(pixScales.x * p.xyz)),
        hash3(floor(pixScales.y * p.xyz)));
    float lerpFactor = frac(log2(pixScale));
    float x = (1.0 - lerpFactor) * a.x + lerpFactor * a.y;
    float t = min(lerpFactor, 1.0 - lerpFactor);
    float3 cases = float3(
        x * x / (2.0 * t * (1.0 - t)),
        (x - 0.5 * t) / (1.0 - t),
        1.0 - (1.0 - x) * (1.0 - x) / (2.0 * t * (1.0 - t)));
    float threshold = (x < 1.0 - t) ? ((x < t) ? cases.x : cases.y) : cases.z;
    threshold = clamp(threshold, 1.0e-6, 1.0);
    return alpha < threshold;
}
