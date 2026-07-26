// The deferred resolve: read the G-buffer, light once per pixel, write the
// scene output.
//
// This is the Vulkan counterpart of glsl/f_pbr_combine.glsl, and at present it
// is a fraction of it. What exists: albedo, packed eye normals, lightmap,
// self-illumination, and a single directional light. What does not: the scene
// light list, shadow lookups, the real PBR BRDF, fog, and the geometry feature
// bits packed into the lightmap alpha. Those arrive with section 4.1 of the
// plan.
//
// The important part is the shape rather than the lighting. SurfaceParams is
// filled here from a G-buffer, and a path tracer will fill the same struct from
// a hit record; everything downstream of it is shared. Keep that boundary.

import uniforms;
import lib.surface;

[[vk::binding(1, 1)]] Sampler2D sGBufDiffuse;
[[vk::binding(2, 1)]] Sampler2D sGBufEyeNormal;
[[vk::binding(3, 1)]] Sampler2D sGBufLightmap;
[[vk::binding(4, 1)]] Sampler2D sGBufSelfIllum;

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Varyings resolveVertex(uint vertexId: SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    Varyings output;
    output.position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

[shader("fragment")]
float4 resolveFragment(Varyings input) : SV_Target {
    float4 diffuseSample = sGBufDiffuse.Sample(input.uv);
    float4 lightmapSample = sGBufLightmap.Sample(input.uv);
    float3 selfIllum = sGBufSelfIllum.Sample(input.uv).rgb;

    // Nothing was drawn here: leave the clear colour alone rather than lighting
    // it, which would tint the whole background.
    if (diffuseSample.a == 0.0f && lightmapSample.a == 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    SurfaceParams surface = defaultSurface();
    surface.albedo = diffuseSample.rgb;
    surface.alpha = diffuseSample.a;
    // Packed into unorm on the way in, so unpack rather than normalise blindly.
    surface.normal = normalize(sGBufEyeNormal.Sample(input.uv).rgb * 2.0f - 1.0f);
    surface.lightmap = lightmapSample.rgb;
    surface.emissive = selfIllum;

    // Placeholder lighting: one fixed direction plus ambient, so geometry reads
    // as shaped rather than flat while the real light list is ported.
    float ndotl = saturate(dot(surface.normal, normalize(float3(0.35f, 0.8f, 0.45f))));
    float3 lit = surface.albedo * (0.35f + 0.65f * ndotl);

    // A lightmap, where present, is treated as albedo for now - see section
    // 16.2 of the plan.
    if (any(surface.lightmap > 0.0f)) {
        lit = surface.albedo * surface.lightmap;
    }

    return float4(lit + surface.emissive, 1.0f);
}
