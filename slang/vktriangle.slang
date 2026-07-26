// A triangle that covers the screen, coloured from a uniform block.
//
// The smallest thing that exercises the whole Vulkan path end to end: a
// pipeline, a descriptor set, a dynamic offset into the per-frame uniform
// arena, and a shader that reads it. Nothing here is used by the game.
//
// It also uses SV_VertexID deliberately. That lowers to the Vulkan VertexIndex
// builtin, which OpenGL's SPIR-V path silently read as zero - the fault that
// stopped the Slang shaders working on GL and prompted the move to Vulkan (see
// section 9.5 of doc/vulkan-rt-backend.md). If this draws, the builtin works.

import uniforms;

struct Varyings {
    float4 position : SV_Position;
};

[shader("vertex")]
Varyings triangleVertex(uint vertexId: SV_VertexID) {
    // Three vertices covering the viewport, no vertex buffer involved.
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);

    Varyings output;
    // Scaled down so the cleared background stays visible around it: a
    // full-screen triangle would prove the draw happened but hide the clear.
    output.position = float4((uv * 2.0f - 1.0f) * 0.7f, 0.0f, 1.0f);
    return output;
}

[shader("fragment")]
float4 triangleFragment(Varyings input) : SV_Target {
    return localUniforms.color;
}
