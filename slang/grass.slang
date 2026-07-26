// Grass, for both the PBR and retro pipelines. Rewritten from glsl/v_grass,
// f_pbr_grass and f_rtr_grass against the architecture in section 9.
//
// One vertex stage feeds two fragment stages, so all three share the varyings
// struct declared here - the coupling is within this file rather than across the
// shader tree.

import uniforms;
import lib.gbuf;
import lib.hashedalpha;
import lib.motion;
import lib.surface;

[[vk::binding(0)]] Sampler2D sMainTex;
[[vk::binding(1)]] Sampler2D sLightmap;

struct Varyings {
    float4 position : SV_Position;
    float4 objectPos : TEXCOORD0;
    float4 worldPos : TEXCOORD1;
    float3 worldNormal : TEXCOORD2;
    float2 uv1 : TEXCOORD3;
    nointerpolation int instanceId : TEXCOORD4;
    float4 curClipPos : TEXCOORD5;
    float4 prevClipPos : TEXCOORD6;
};

[shader("vertex")]
Varyings grassVertex(
    [[vk::location(0)]] float3 position: POSITION,
    [[vk::location(2)]] float2 uv1: TEXCOORD0,
    uint instanceId: SV_InstanceID,
    uint startInstance: SV_StartInstanceLocation) {
    // Slang lowers SV_InstanceID to InstanceIndex - BaseInstance, matching D3D.
    // Under OpenGL that evaluates to zero for every instance, so all the grass
    // stacks on one cluster and the field vanishes. Adding the base back cancels
    // the subtraction and leaves the true index, whatever BaseInstance reports.
    int instanceIndex = int(instanceId + startInstance);
    Varyings output;

    float3 clusterToCamera = grassUniforms.clusters[instanceIndex].positionVariant.xyz -
                             globalUniforms.cameraPosition.xyz;
    float A = asin(smoothstep(0.5 * grassUniforms.radius, grassUniforms.radius, length(clusterToCamera)));

    // The original builds a pitch rotation about X, multiplies it by the view
    // matrix, and takes the first two rows as the billboard axes. The product is
    // expanded here rather than written as a matrix multiply: only mul(matrix,
    // vector) has been verified against the reference renderer, and a
    // matrix-matrix product would additionally depend on how the row-major
    // storage qualifier interacts with row extraction.
    //
    // For a rotation about X, row 0 is untouched and row 1 is a blend of the
    // view's rows 1 and 2.
    float3 viewRow0 = globalUniforms.view[0].xyz;
    float3 viewRow1 = globalUniforms.view[1].xyz;
    float3 viewRow2 = globalUniforms.view[2].xyz;
    float3 right = viewRow0;
    float3 up = cos(A) * viewRow1 - sin(A) * viewRow2;

    output.objectPos = float4(position, 1.0);
    output.worldPos = float4(
        grassUniforms.clusters[instanceIndex].positionVariant.xyz +
            right * position.x * grassUniforms.quadSize.x +
            up * position.y * grassUniforms.quadSize.y,
        1.0);

    // Clusters are static, so only the camera contributes motion. The billboard
    // turning to face it is deliberately not tracked.
    output.curClipPos = mul(globalUniforms.viewProjection, output.worldPos);
    output.prevClipPos = mul(globalUniforms.prevViewProjection, output.worldPos);

    output.worldNormal = cross(right, up);
    output.uv1 = uv1;
    output.instanceId = instanceIndex;
    output.position = mul(globalUniforms.projection, mul(globalUniforms.view, output.worldPos));
    return output;
}

/** Grass sheets pack four variants into one texture, chosen per cluster. */
float2 variantUV(Varyings input) {
    int variant = int(grassUniforms.clusters[input.instanceId].positionVariant[3]);
    float2 uv = 0.5 * input.uv1;
    uv.y += 0.5 * (variant / 2);
    uv.x += 0.5 * (variant % 2);
    return uv;
}

SurfaceParams sampleSurface(Varyings input) {
    SurfaceParams surface = defaultSurface();
    float4 mainTexSample = sMainTex.Sample(variantUV(input));
    surface.albedo = mainTexSample.rgb;
    surface.alpha = mainTexSample.a;
    surface.normal = normalize(input.worldNormal);
    if (isFeatureEnabled(kFeatureLightmap)) {
        surface.lightmap = sLightmap.Sample(grassUniforms.clusters[input.instanceId].lightmapUV).rgb;
        surface.hasLightmap = true;
    }
    return surface;
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
    SurfaceParams surface = sampleSurface(input);
    if (hashedAlphaTestFails(surface.alpha, input.objectPos.xyz)) {
        discard;
    }

    float features = packGeometryFeatures(false, false, false);
    float3 eyeNormal = mul(surface.normal, (float3x3)globalUniforms.viewInv);
    eyeNormal = 0.5 * eyeNormal + 0.5;

    GBufferOutput output;
    output.diffuseColor = float4(surface.albedo, surface.alpha);
    output.lightmapColor = float4(surface.lightmap, features);
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
    SurfaceParams surface = sampleSurface(input);
    if (hashedAlphaTestFails(surface.alpha, input.objectPos.xyz)) {
        discard;
    }

    RetroOutput output;
    output.color = float4(surface.albedo * surface.lightmap, 1.0);
    output.hilights = float4(0.0);
    return output;
}
