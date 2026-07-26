// A Mesh drawn with its real vertex attributes.
//
// Takes position, normal and uv1 at the locations Mesh::VertexLayout and the
// GL path already use, so the same interleaved buffer feeds both backends
// unchanged. Shades by normal so a wrong stride or a wrong attribute offset
// shows up as obviously wrong geometry rather than a plausible flat colour.
//
// Nothing here is used by the game; it exists to prove the upload path.

import uniforms;

[[vk::binding(0, 1)]] Sampler2D sMainTex;

struct Varyings {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Varyings meshVertex(
    [[vk::location(0)]] float3 position: POSITION,
    [[vk::location(1)]] float3 normal: NORMAL,
    [[vk::location(2)]] float2 uv1: TEXCOORD0) {
    Varyings output;
    output.position = mul(globalUniforms.viewProjection,
                          mul(localUniforms.model, float4(position, 1.0f)));
    output.normal = mul(localUniforms.model, float4(normal, 0.0f)).xyz;
    output.uv = uv1;
    return output;
}

[shader("fragment")]
float4 meshFragment(Varyings input) : SV_Target {
    // A fixed light direction, so the shape reads without a light set up.
    float ndotl = saturate(dot(normalize(input.normal), normalize(float3(0.4f, 0.6f, 1.0f))));
    float3 albedo = sMainTex.Sample(input.uv).rgb * localUniforms.color.rgb;
    return float4(albedo * (0.25f + 0.75f * ndotl), 1.0f);
}
