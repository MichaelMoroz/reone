module oit;

import uniforms;

/** What the two OIT targets take: weighted colour with revealage, and the weight. */
public struct OITOutput {
    /** rgb = colour times weight, a = the fragment's own coverage. */
    public float4 accum : SV_Target0;
    /** The weight again, so the resolve can normalise by their sum. */
    public float4 revealage : SV_Target1;
};

/**
 * The weight a fragment carries into weighted-blended transparency, ported
 * from glsl/i_oit.glsl.
 *
 * Near surfaces count for more than far ones, which is what lets a stack of
 * transparent surfaces resolve to something close to sorted without being
 * sorted. The hundred is the falloff distance the original chose.
 *
 * @param depth window-space depth. OpenGL's gl_FragCoord.z and Vulkan's
 *              SV_Position.z are the same 0..1 value once the projection has
 *              been through glToVulkanClip, so the two backends agree here.
 */
public float oitWeight(float depth, float alpha) {
    float near = globalUniforms.clipNear;
    float far = globalUniforms.clipFar;
    float eyeZ = (near * far) / ((near - far) * depth + far);
    return alpha * (1.0 / (1.0 + abs(eyeZ) / 100.0));
}

/**
 * Split shaded colour and coverage across the pair of targets.
 *
 * The blend does the rest: colour sums additively, and alpha - starting from a
 * clear of one - is multiplied down by one minus coverage on every fragment,
 * which is what makes accum's alpha channel revealage.
 */
public OITOutput packOIT(float3 color, float alpha, float depth) {
    float w = oitWeight(depth, alpha);
    OITOutput output;
    output.accum = float4(color * w, alpha);
    output.revealage = float4(w);
    return output;
}
