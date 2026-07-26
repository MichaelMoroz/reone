// A triangle that samples a texture, tinted from a uniform block.
//
// The smallest thing that exercises the whole Vulkan path end to end: a
// pipeline, both descriptor sets, a dynamic offset into the per-frame uniform
// arena, an uploaded image and a sampler. Nothing here is used by the game.
//
// It also uses SV_VertexID deliberately. That lowers to the Vulkan VertexIndex
// builtin, which OpenGL's SPIR-V path silently read as zero - the fault that
// stopped the Slang shaders working on GL and prompted the move to Vulkan (see
// section 9.5 of doc/vulkan-rt-backend.md). If this draws, the builtin works.

import uniforms;

// Set 1. Uniform blocks and textures both number from zero, so they cannot
// share a set; see VulkanDescriptors::kTextureSet.
[[vk::binding(0, 1)]] Sampler2D sMainTex;

struct Varyings {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Varyings triangleVertex(uint vertexId: SV_VertexID) {
    // Three vertices covering the viewport, no vertex buffer involved.
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);

    Varyings output;
    // Scaled down so the cleared background stays visible around it: a
    // full-screen triangle would prove the draw happened but hide the clear.
    output.position = float4((uv * 2.0f - 1.0f) * 0.7f, 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

[shader("fragment")]
float4 triangleFragment(Varyings input) : SV_Target {
    return sMainTex.Sample(input.uv) * localUniforms.color;
}
