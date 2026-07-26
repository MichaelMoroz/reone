module motion;

/**
 * Screen-space motion vectors. Ported from glsl/i_motion.glsl.
 *
 * Clip positions must come from the unjittered view-projection matrices so that
 * the sub-pixel jitter does not leak into the result. The vector points from
 * where the surface was to where it is now, as a UV-space delta.
 */
public float2 computeMotion(float4 curClipPos, float4 prevClipPos) {
    if (curClipPos.w <= 0.0 || prevClipPos.w <= 0.0) {
        return float2(0.0);
    }
    float2 cur = curClipPos.xy / curClipPos.w;
    float2 prev = prevClipPos.xy / prevClipPos.w;
    return 0.5 * (cur - prev);
}
