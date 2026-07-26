// Opaque model, PBR pipeline. Ported from glsl/v_model.glsl and
// glsl/f_pbr_opaqmodel.glsl as the first proof that Slang can express this
// engine's shaders and generate GLSL that renders identically.
//
// Matrix convention: slangc is invoked with -matrix-layout-column-major, which
// emits GLSL `layout(row_major)` and `v * M`. Reading glm's column-major memory
// as row-major transposes it, and `v * M` transposes again, so mul(M, v) ends up
// meaning M * v exactly as the original GLSL did. The two inversions cancel.
// Where the original relies on that, it is spelled out below.

import uniforms;
import lib.gbuf;
import lib.hashedalpha;
import lib.math;
import lib.motion;

// Binding indices are texture units - see TextureUnits in reone/graphics/types.h.
[[vk::binding(0)]] Sampler2D sMainTex;
[[vk::binding(1)]] Sampler2D sLightmap;
[[vk::binding(2)]] Sampler2D sEnvMap;
[[vk::binding(3)]] Sampler2D sNormalMap;
[[vk::binding(14)]] Sampler2DArray sBumpMapArray;
[[vk::binding(18)]] SamplerCube sEnvMapCube;

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv1 : TEXCOORD0;
    [[vk::location(3)]] float2 uv2 : TEXCOORD1;
    [[vk::location(4)]] float3 bitangent : BINORMAL;
    [[vk::location(5)]] float3 tangent : TANGENT;
    [[vk::location(6)]] float3 tanSpaceNormal : TEXCOORD2;
    [[vk::location(7)]] float4 boneIndices : BLENDINDICES;
    [[vk::location(8)]] float4 boneWeights : BLENDWEIGHT;
    uint vertexId : SV_VertexID;
};

// Shared between the two stages so that varying locations cannot drift apart.
// The tangent frame is carried as three vectors rather than a matrix, to keep it
// clear of any matrix convention question.
struct Varyings {
    float4 position : SV_Position;
    float4 objectPos : TEXCOORD0;
    float4 worldPos : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float2 uv1 : TEXCOORD3;
    float2 uv2 : TEXCOORD4;
    float3 tangent : TEXCOORD5;
    float3 bitangent : TEXCOORD6;
    float3 tanSpaceNormal : TEXCOORD7;
    float4 curClipPos : TEXCOORD8;
    float4 prevClipPos : TEXCOORD9;
};

[shader("vertex")]
Varyings vertexMain(VSInput input) {
    Varyings output;

    float4 P = float4(input.position, 1.0);
    float4 N = float4(input.normal, 0.0);
    float4 prevP = P;

    if (isFeatureEnabled(kFeatureSkin)) {
        int i1 = max(0, int(input.boneIndices[0]));
        int i2 = max(0, int(input.boneIndices[1]));
        int i3 = max(0, int(input.boneIndices[2]));
        int i4 = max(0, int(input.boneIndices[3]));

        float w1 = input.boneWeights[0];
        float w2 = input.boneWeights[1];
        float w3 = input.boneWeights[2];
        float w4 = input.boneWeights[3];

        P = mul(boneUniforms.bones[i1], P) * w1 +
            mul(boneUniforms.bones[i2], P) * w2 +
            mul(boneUniforms.bones[i3], P) * w3 +
            mul(boneUniforms.bones[i4], P) * w4;

        N = mul(boneUniforms.bones[i1], N) * w1 +
            mul(boneUniforms.bones[i2], N) * w2 +
            mul(boneUniforms.bones[i3], N) * w3 +
            mul(boneUniforms.bones[i4], N) * w4;

        prevP = mul(boneUniforms.prevBones[i1], prevP) * w1 +
                mul(boneUniforms.prevBones[i2], prevP) * w2 +
                mul(boneUniforms.prevBones[i3], prevP) * w3 +
                mul(boneUniforms.prevBones[i4], prevP) * w4;

        output.objectPos = P;

    } else if (isFeatureEnabled(kFeatureDangly)) {
        output.objectPos = danglyUniforms.positions[input.vertexId];
        prevP = output.objectPos;

    } else if (isFeatureEnabled(kFeatureSaber)) {
        float signum = 2.0 * (input.vertexId / 88) - 1.0;
        float hdist = ((input.vertexId % 88) / 4) / 21.0;
        float vdist = (input.vertexId % 4) / 3.0;
        output.objectPos = float4(P.xyz + 0.5 * signum * hdist * vdist * localUniforms.saberDisplacement.xyz, 1.0);
        prevP = output.objectPos;

    } else {
        output.objectPos = P;
    }

    output.worldPos = mul(localUniforms.model, output.objectPos);

    // The original is `transpose(mat3(uModelInv)) * N`. mul(v, M) is the
    // row-vector product, which is transpose(M) * v, so this is the same thing
    // without constructing the transpose.
    float3x3 modelInv3 = (float3x3)localUniforms.modelInv;
    output.worldNormal = normalize(mul(N.xyz, modelInv3));

    output.uv1 = input.uv1;
    output.uv2 = input.uv2;

    if (isFeatureEnabled(kFeatureNormalMap) || isFeatureEnabled(kFeatureBumpMap)) {
        output.tangent = normalize(mul(input.tangent, modelInv3));
        output.bitangent = normalize(mul(input.bitangent, modelInv3));
        output.tanSpaceNormal = normalize(mul(input.tanSpaceNormal, modelInv3));
    } else {
        output.tangent = float3(0.0);
        output.bitangent = float3(0.0);
        output.tanSpaceNormal = float3(0.0);
    }

    output.curClipPos = mul(globalUniforms.viewProjection, output.worldPos);
    output.prevClipPos = mul(globalUniforms.prevViewProjection, mul(localUniforms.prevModel, prevP));

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

/** The original builds mat3(T, B, TSN), whose columns are the three vectors. */
float3 tangentToWorld(Varyings input, float3 n) {
    return input.tangent * n.x + input.bitangent * n.y + input.tanSpaceNormal * n.z;
}

float3 getNormal(Varyings input, float2 uv) {
    if (isFeatureEnabled(kFeatureNormalMap)) {
        float3 n = sNormalMap.Sample(uv).rgb * 2.0 - 1.0;
        return tangentToWorld(input, normalize(n));
    } else if (isFeatureEnabled(kFeatureBumpMap)) {
        float height = sBumpMapArray.Sample(float3(uv, localUniforms.bumpMapFrame)).r;
        float dX = localUniforms.bumpMapScale * ddx(height);
        float dY = localUniforms.bumpMapScale * ddy(height);
        float3 n = normalize(float3(-dX, -dY, 1.0));
        return tangentToWorld(input, normalize(n));
    } else {
        return normalize(input.worldNormal);
    }
}

[shader("fragment")]
GBufferOutput fragmentMain(Varyings input) {
    // uUV is a mat3 in GLSL, held as three float4 columns here; the original
    // computes vec2(uUV * vec3(uv1, 1.0)).
    float3 uvHom = float3(input.uv1, 1.0);
    float2 uv = float2(
        localUniforms.uv[0].x * uvHom.x + localUniforms.uv[1].x * uvHom.y + localUniforms.uv[2].x * uvHom.z,
        localUniforms.uv[0].y * uvHom.x + localUniforms.uv[1].y * uvHom.y + localUniforms.uv[2].y * uvHom.z);

    float3 normal = getNormal(input, uv);
    float4 mainTexSample = sMainTex.Sample(uv);

    if (!isFeatureEnabled(kFeatureEnvMap)) {
        if (isFeatureEnabled(kFeatureHashedAlphaTest)) {
            if (hashedAlphaTestFails(mainTexSample.a, input.objectPos.xyz)) {
                discard;
            }
        } else if (mainTexSample.a == 0.0) {
            discard;
        }
    }

    float4 diffuseColor = mainTexSample;
    if (isFeatureEnabled(kFeatureWater)) {
        diffuseColor *= localUniforms.waterAlpha;
    }

    float features = packGeometryFeatures(
        isFeatureEnabled(kFeatureEnvMapCube),
        isFeatureEnabled(kFeatureShadows),
        isFeatureEnabled(kFeatureFog));

    // As above: the row-vector product stands in for transpose(mat3(uViewInv)).
    float3 eyeNormal = mul(normal, (float3x3)globalUniforms.viewInv);
    eyeNormal = 0.5 * eyeNormal + 0.5;

    GBufferOutput output;
    output.diffuseColor = diffuseColor;
    output.lightmapColor = isFeatureEnabled(kFeatureLightmap)
                               ? float4(sLightmap.Sample(input.uv2).rgb, features)
                               : float4(float3(1.0), features);
    output.selfIllumColor = float4(localUniforms.selfIllumColor.rgb, localUniforms.envMapDerivedLayer / 255.0);
    output.eyeNormal = float4(eyeNormal, 0.0);
    output.motion = computeMotion(input.curClipPos, input.prevClipPos);
    return output;
}
