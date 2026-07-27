// Shared vertex and fragment stages that several programs are built from.
// Ported from glsl/v_passthrough, v_mvp, v_billboard, v_aabb, f_color, f_null,
// f_texture and f_texnoper.
//
// See slang/pbr_opaque_model.slang for the matrix convention these rely on.

import uniforms;

[[vk::binding(0, 1)]] Sampler2D sMainTex;

/** glm::mat3x4 held as three float4 columns; the original is vec2(uUV * vec3(uv, 1)). */
float2 applyUVTransform(float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(
        localUniforms.uv[0].x * h.x + localUniforms.uv[1].x * h.y + localUniforms.uv[2].x * h.z,
        localUniforms.uv[0].y * h.x + localUniforms.uv[1].y * h.y + localUniforms.uv[2].y * h.z);
}

// ---------------------------------------------------------------- passthrough

struct PassthroughVaryings {
    float4 position : SV_Position;
    noperspective float2 uv1 : TEXCOORD0;
};

[shader("vertex")]
PassthroughVaryings passthroughVertex(
    [[vk::location(0)]] float3 position: POSITION,
    [[vk::location(2)]] float2 uv1: TEXCOORD0) {
    PassthroughVaryings output;
    output.uv1 = uv1;
    output.position = float4(position, 1.0);
    return output;
}

// ------------------------------------------------------------------------ mvp

/**
 * Shared by the mvp and billboard stages because both are paired with
 * textureFragment. GLSL matched stage interfaces by name, which let the two
 * disagree on ordering; Slang matches by location, so they must not.
 */
struct MVPVaryings {
    float4 position : SV_Position;
    float4 worldPos : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

[shader("vertex")]
MVPVaryings mvpVertex(
    [[vk::location(0)]] float3 position: POSITION,
    [[vk::location(2)]] float2 uv1: TEXCOORD0) {
    MVPVaryings output;
    output.worldPos = mul(localUniforms.model, float4(position, 1.0));
    output.uv1 = uv1;
    output.position = mul(globalUniforms.projection, mul(globalUniforms.view, output.worldPos));
    return output;
}

// ------------------------------------------------------------------ billboard

[shader("vertex")]
MVPVaryings billboardVertex(
    [[vk::location(0)]] float3 position: POSITION,
    [[vk::location(2)]] float2 uv1: TEXCOORD0) {
    MVPVaryings output;
    output.worldPos = float4(0.0);

    if (isFeatureEnabled(kFeatureFixedSize)) {
        float4 clip = mul(globalUniforms.projection,
                          mul(globalUniforms.view,
                              mul(localUniforms.model, float4(0.0, 0.0, 0.0, 1.0))));
        clip /= clip.w;
        clip.xy += localUniforms.billboardSize * position.xy;
        output.position = clip;
    } else {
        // The original reads the view matrix's first two rows as the camera's
        // right and up axes: vec3(uView[0][0], uView[1][0], uView[2][0]) picks
        // one component from each column, which is a row.
        float4x4 view = globalUniforms.view;
        float3 right = float3(view[0][0], view[0][1], view[0][2]);
        float3 up = float3(view[1][0], view[1][1], view[1][2]);
        float4x4 model = localUniforms.model;
        // uModel[3] is the translation column; under this convention it is row 3.
        float3 origin = float3(model[0][3], model[1][3], model[2][3]);
        float4 P = float4(
            origin + right * model[0][0] * position.x + up * model[1][1] * position.y,
            1.0);
        output.position = mul(globalUniforms.projection, mul(globalUniforms.view, P));
    }

    output.uv1 = uv1;
    return output;
}

// ----------------------------------------------------------------------- aabb

struct AABBVaryings {
    float4 position : SV_Position;
    float4 worldPos : TEXCOORD0;
    float4 curClipPos : TEXCOORD1;
    float4 prevClipPos : TEXCOORD2;
};

[shader("vertex")]
AABBVaryings aabbVertex(uint vertexId: SV_VertexID) {
    AABBVaryings output;
    output.worldPos = aabbUniforms.corners[vertexId];
    // Debug geometry, rebuilt every frame in world space, so only the camera moves.
    output.curClipPos = mul(globalUniforms.viewProjection, output.worldPos);
    output.prevClipPos = mul(globalUniforms.prevViewProjection, output.worldPos);
    output.position = mul(globalUniforms.projection, mul(globalUniforms.view, output.worldPos));
    return output;
}

// -------------------------------------------------------------------- colour

[shader("fragment")]
float4 colorFragment() : SV_Target {
    return localUniforms.color;
}

struct RetroAABBOutput {
    float4 color : SV_Target0;
    float4 hilights : SV_Target1;
};

/** A debug box still has two attachments in the retro forward pass. */
[shader("fragment")]
RetroAABBOutput retroAABBFragment(AABBVaryings input) {
    RetroAABBOutput output;
    output.color = float4(1.0);
    output.hilights = float4(0.0);
    return output;
}

// ----------------------------------------------------------------------- null

[shader("fragment")]
void nullFragment() {
}

// -------------------------------------------------------------------- texture

[shader("fragment")]
float4 textureFragment(MVPVaryings input) : SV_Target {
    float2 uv = applyUVTransform(input.uv1);
    float4 mainTexSample = sMainTex.Sample(uv);
    return float4(localUniforms.color.rgb * mainTexSample.rgb,
                  localUniforms.color.a * mainTexSample.a);
}

[shader("fragment")]
float4 textureNoPerspectiveFragment(noperspective float2 uv1: TEXCOORD0) : SV_Target {
    float2 uv = applyUVTransform(uv1);
    float4 mainTexSample = sMainTex.Sample(uv);
    return float4(localUniforms.color.rgb * mainTexSample.rgb,
                  localUniforms.color.a * mainTexSample.a);
}
