/**
 * Screen-space motion vectors.
 *
 * Clip positions must be computed with the unjittered uViewProjection and
 * uPrevViewProjection, so that the sub-pixel jitter does not leak into the
 * result. The vector points from where the surface was to where it is now,
 * expressed as a UV-space delta.
 */
vec2 computeMotion(vec4 curClipPos, vec4 prevClipPos) {
    if (curClipPos.w <= 0.0 || prevClipPos.w <= 0.0) {
        return vec2(0.0);
    }
    vec2 cur = curClipPos.xy / curClipPos.w;
    vec2 prev = prevClipPos.xy / prevClipPos.w;
    return 0.5 * (cur - prev);
}
