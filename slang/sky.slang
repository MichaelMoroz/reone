// Path-traced sky-room capture. This is deliberately a complete, tiny shader
// program rather than another material-pipeline entry point: the sky is unlit and its
// original mesh buffers provide only the position and primary UV it needs.

import uniforms;

[[vk::binding(0, 1)]] Sampler2D sMainTex;

struct SkyVertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct SkyVaryings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
SkyVaryings skyVertex(SkyVertexInput input) {
    SkyVaryings output;
    float4 worldPosition = mul(localUniforms.model, float4(input.position, 1.0));
    output.position = mul(globalUniforms.projection,
                          mul(globalUniforms.view, worldPosition));
    output.uv = input.uv;
    return output;
}

float2 transformedUV(float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(
        localUniforms.uv[0].x * h.x + localUniforms.uv[1].x * h.y + localUniforms.uv[2].x * h.z,
        localUniforms.uv[0].y * h.x + localUniforms.uv[1].y * h.y + localUniforms.uv[2].y * h.z);
}

[shader("fragment")]
float4 skyFragment(SkyVaryings input) : SV_Target {
    float4 sample = sMainTex.Sample(transformedUV(input.uv));
    if (sample.a == 0.0) discard;
    // The cube is sampled as radiance by the tracer. Preserve the old bake's
    // explicit gamma conversion exactly.
    return float4(pow(sample.rgb, float3(2.2)), sample.a);
}
