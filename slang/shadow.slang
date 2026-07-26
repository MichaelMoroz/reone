// Shadow map rendering, ported from glsl/v_shadows, g_dirlightshadow,
// g_ptlightshadow, f_null and f_ptlightshadow.
//
// The OpenGL pipeline uses a geometry shader to fan each triangle across the
// four directional cascades or the six cube faces, setting gl_Layer per copy.
// Here that is multiview instead: the pass declares a view mask, the vertex
// stage runs once per view, and SV_ViewID says which one - no geometry stage,
// and the driver is free to broadcast rather than re-run.

import uniforms;

struct Varyings {
    float4 position : SV_Position;
    float4 worldPos : TEXCOORD0;
};

/**
 * One vertex stage for both light kinds.
 *
 * shadowLightSpace holds four cascade matrices for a directional light or six
 * face matrices for a point light, so the view index selects the right one
 * either way. World position is carried through for the point-light fragment
 * stage, which needs a distance rather than a depth.
 */
[shader("vertex")]
Varyings shadowVertex(
    [[vk::location(0)]] float3 position: POSITION,
    uint viewId: SV_ViewID) {
    Varyings output;
    output.worldPos = mul(localUniforms.model, float4(position, 1.0));
    output.position = mul(globalUniforms.shadowLightSpace[viewId], output.worldPos);
    return output;
}

/**
 * Directional cascades need no fragment stage at all - the depth attachment is
 * the entire output. Slang has no empty fragment entry, so this is the nearest
 * thing: a stage that writes no colour and lets fixed-function depth do its job.
 */
[shader("fragment")]
void nullFragment(Varyings input) {
}

/**
 * Point lights store radial distance rather than projected depth, so that the
 * lookup can compare against the length of the fragment-to-light vector without
 * reconstructing anything.
 *
 * The 2500 divisor matches the OpenGL shader, and the lookup multiplies by the
 * same constant. It is a fixed far plane for shadow-casting point lights.
 */
[shader("fragment")]
float pointFragment(Varyings input) : SV_Depth {
    float lightDistance = length(input.worldPos.xyz - globalUniforms.shadowLightPosition.xyz);
    return lightDistance / 2500.0;
}
