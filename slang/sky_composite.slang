// The one place the sky reaches the raster image. Retro and PBR both shade a
// single G-buffer, so the sky is a single pass between their resolve and the
// transparent pass rather than a term inside either resolve - an earlier
// integration had it in the PBR resolve and in postprocess at once, which is
// two implementations of one idea, and postprocess sits past transparency
// where it would paint over particles and lens flares.
//
// The bake stores linear radiance - pow(texel, 2.2) of the authored skybox
// faces - so inverting that here reproduces the geometry sky's texels exactly,
// keeping the raster frame's authored look. The tracer applies its own display
// transform to the same bake; reconciling the two levels is the shared
// post-process pass's job, not this one's.
//
// Nothing here is mode-specific: retro and PBR both write gamma-space colour
// into the scene output, and both take this sky.

import uniforms;

[[vk::binding(5, 1)]] Sampler2D sGBufDepth;
[[vk::binding(18, 1)]] SamplerCube sSkyCube;

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Varyings skyCompositeVertex(uint vertexId : SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Varyings output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

/**
 * The world-space ray through this pixel, from the same reconstruction the two
 * resolves use: sGBufDepth is device depth in [0,1] and goes into NDC
 * unchanged, and NDC y undoes the geometry pass's negative-height viewport.
 * Taking it at the far plane gives a point on the ray rather than on a
 * surface, so the direction is exact without a separate camera basis.
 */
float3 skyDirection(float2 uv) {
    float3 ndc = float3(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, 1.0);
    float4 viewPos = mul(globalUniforms.projectionInv, float4(ndc, 1.0));
    float3 worldPos = mul(globalUniforms.viewInv, float4(viewPos.xyz / viewPos.w, 1.0)).xyz;
    return normalize(worldPos - globalUniforms.cameraPosition.xyz);
}

[shader("fragment")]
float4 skyCompositeFragment(Varyings input) : SV_Target {
    // Far depth is the whole test. Discarding leaves the resolved pixel as it
    // was, so the changed set is exactly the set nothing wrote depth into.
    float depth = sGBufDepth.Sample(input.uv).r;
    if (depth < 1.0) discard;
    // Undo the bake's linearisation: the result is the authored texel the
    // geometry sky would have drawn, in the gamma space the frame is in.
    float3 radiance = sSkyCube.SampleLevel(skyDirection(input.uv), 0.0).rgb;
    return float4(pow(max(radiance, 0.0), 1.0 / 2.2), 1.0);
}
