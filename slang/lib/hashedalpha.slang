module hashedalpha;

// Hashed alpha testing, as used for foliage and other punch-through surfaces.
// Ported from glsl/i_hash.glsl and glsl/i_hashedalpha.glsl.

float hash2(float2 p) {
    return frac(1.0e4 * sin(17.0 * p.x + 0.1 * p.y) * (0.1 + abs(sin(13.0 * p.y + p.x))));
}

float hash3(float3 p) {
    return hash2(float2(hash2(p.xy), p.z));
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
