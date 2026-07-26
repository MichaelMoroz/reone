// Walkmesh and trigger debug geometry, for both pipelines. Rewritten from
// glsl/v_walkmesh, f_pbr_walkmesh and f_rtr_walkmesh.

import uniforms;
import lib.gbuf;
import lib.geometry;
import lib.motion;

struct Varyings {
    float4 position : SV_Position;
    float4 worldPos : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    nointerpolation int material : TEXCOORD2;
    float4 curClipPos : TEXCOORD3;
    float4 prevClipPos : TEXCOORD4;
};

static const int kMaxWalkmeshMaterialsShader = 32;

[shader("vertex")]
Varyings walkmeshVertex(
    [[vk::location(0)]] float3 position: POSITION,
    [[vk::location(1)]] float3 normal: NORMAL,
    [[vk::location(9)]] float material: TEXCOORD3) {
    Varyings output;
    output.worldPos = mul(localUniforms.model, float4(position, 1.0));
    output.worldNormal = transformNormal(normalize(normal));
    output.material = int(material * (kMaxWalkmeshMaterialsShader - 1));

    output.curClipPos = mul(globalUniforms.viewProjection, output.worldPos);
    output.prevClipPos = mul(globalUniforms.prevViewProjection,
                             mul(localUniforms.prevModel, float4(position, 1.0)));

    output.position = mul(globalUniforms.projection, mul(globalUniforms.view, output.worldPos));
    return output;
}

struct GBufferOutput {
    float4 diffuseColor : SV_Target0;
    float4 eyeNormal : SV_Target1;
    float4 lightmapColor : SV_Target2;
    float4 selfIllumColor : SV_Target3;
    float2 motion : SV_Target4;
};

[shader("fragment")]
GBufferOutput pbrFragment(Varyings input) {
    float3 eyeNormal = mul(normalize(input.worldNormal), (float3x3)globalUniforms.viewInv);
    eyeNormal = 0.5 * eyeNormal + 0.5;

    GBufferOutput output;
    output.diffuseColor = float4(walkmeshUniforms.materials[input.material].rgb, 1.0);
    output.lightmapColor = float4(float3(1.0), 0.0);
    output.selfIllumColor = float4(0.0);
    output.eyeNormal = float4(eyeNormal, 0.0);
    output.motion = computeMotion(input.curClipPos, input.prevClipPos);
    return output;
}

struct RetroOutput {
    float4 color : SV_Target0;
    float4 hilights : SV_Target1;
};

[shader("fragment")]
RetroOutput retroFragment(Varyings input) {
    RetroOutput output;
    output.color = float4(walkmeshUniforms.materials[input.material].rgb, 1.0);
    output.hilights = float4(0.0);
    return output;
}
