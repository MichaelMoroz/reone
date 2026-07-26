// Writing and resolving a G-buffer.
//
// Two entry point pairs. The geometry pass writes five attachments in the same
// order as the OpenGL PBR pipeline's fbOpaqueGeometry, so the two can be
// compared attachment by attachment. The resolve reads them back and lights
// once per pixel.
//
// Nothing here is used by the game yet; it exists to prove multiple render
// targets, the attachment-to-texture layout transition, and the resolve.

import uniforms;

// Set 1. Only the resolve reads these; the geometry pass samples the diffuse
// map at unit 0.
[[vk::binding(0, 1)]] Sampler2D sMainTex;
[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;

struct Varyings {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

/** Attachment order matches VulkanGBuffer::Attachment. */
struct GBufferOutput {
    float4 diffuse : SV_Target0;
    float4 eyeNormal : SV_Target1;
    float4 lightmap : SV_Target2;
    float4 selfIllum : SV_Target3;
    float2 motion : SV_Target4;
};

[shader("vertex")]
Varyings geometryVertex(
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
GBufferOutput geometryFragment(Varyings input) {
    GBufferOutput output;
    output.diffuse = float4(sMainTex.Sample(input.uv).rgb * localUniforms.color.rgb, 1.0f);
    // Normals are signed and the target is unorm, so they are packed into
    // 0..1 here and unpacked in the resolve.
    output.eyeNormal = float4(normalize(input.normal) * 0.5f + 0.5f, 1.0f);
    output.lightmap = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.selfIllum = float4(0.0f, 0.0f, 0.0f, 1.0f);
    output.motion = float2(0.0f, 0.0f);
    return output;
}

struct ResolveVaryings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
ResolveVaryings resolveVertex(uint vertexId: SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    ResolveVaryings output;
    output.position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

[shader("fragment")]
float4 resolveFragment(ResolveVaryings input) : SV_Target {
    float3 diffuse = sGBufDiffuse.Sample(input.uv).rgb;
    float3 normal = sGBufEyeNormal.Sample(input.uv).rgb * 2.0f - 1.0f;
    float3 selfIllum = sGBufSelfIllum.Sample(input.uv).rgb;

    // A single fixed light, so the resolve demonstrably uses the normal target
    // rather than just copying the diffuse one through.
    float ndotl = saturate(dot(normalize(normal), normalize(float3(0.4f, 0.6f, 1.0f))));
    return float4(diffuse * (0.2f + 0.8f * ndotl) + selfIllum, 1.0f);
}
