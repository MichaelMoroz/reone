// Particles, ported from glsl/v_particles and f_oit_particles.
//
// Two fragment stages over one shading function: the transparency pass takes
// the weighted-blended pair, and anything drawing particles straight onto an
// already-resolved image takes the single colour and leans on the emitter's own
// back-to-front ordering instead.

import uniforms;
import lib.math;
import lib.oit;

[[vk::binding(0, 1)]] Sampler2D sMainTex;

struct Varyings {
    float4 position : SV_Position;
    float2 uv1 : TEXCOORD0;
    nointerpolation int instanceId : TEXCOORD1;
};

[shader("vertex")]
Varyings particleVertex(
    [[vk::location(0)]] float3 position: POSITION,
    [[vk::location(2)]] float2 uv1: TEXCOORD0,
    uint instanceId: SV_InstanceID,
    uint startInstance: SV_StartInstanceLocation) {
    // Adding the base instance back cancels the subtraction Slang lowers
    // SV_InstanceID to; see the Vulkan rationale in grass.slang.
    int instanceIndex = int(instanceId + startInstance);
    ParticleUniformsParticle particle = particleUniforms.particles[instanceIndex];

    // The quad is built in view space from the axes the emitter computed, which
    // is what keeps it facing the camera.
    float3 worldPos = particle.positionFrame.xyz +
                      particle.right.xyz * position.x * particle.size.x +
                      particle.up.xyz * position.y * particle.size.y;

    Varyings output;
    output.position = mul(globalUniforms.projection,
                          mul(globalUniforms.view, float4(worldPos, 1.0)));
    output.uv1 = uv1;
    output.instanceId = instanceIndex;
    return output;
}

/** Animation frames are laid out as a grid within the one texture. */
float2 frameUV(Varyings input) {
    float2 grid = float2(max(particleUniforms.gridSize, int2(1, 1)));
    float2 uv = input.uv1 / grid;

    int frame = int(particleUniforms.particles[input.instanceId].positionFrame.w);
    if (frame > 0) {
        uv.x += (frame % particleUniforms.gridSize.x) / grid.x;
        uv.y += (frame / particleUniforms.gridSize.x) / grid.y;
    }
    return uv;
}

/** Colour and coverage of one particle fragment, before it is written out. */
float4 shadeParticle(Varyings input) {
    ParticleUniformsParticle particle = particleUniforms.particles[input.instanceId];

    float4 mainTexSample = sMainTex.Sample(frameUV(input));
    float3 mainTexColor = mainTexSample.rgb;
    float mainTexAlpha = mainTexSample.a;
    if (isFeatureEnabled(kFeaturePremulAlpha)) {
        // The texture already has colour multiplied by coverage, and carries no
        // usable alpha channel. Luma recovers the coverage, and dividing it back
        // out gives the unpremultiplied colour the blend expects.
        mainTexAlpha = rgbToLuma(mainTexSample.rgb);
        mainTexColor *= 1.0 / max(0.0001, mainTexAlpha);
    }

    float3 color = particle.color.rgb * mainTexColor;
    float alpha = particle.color.a * mainTexAlpha;
    if (alpha == 0.0) {
        discard;
    }
    return float4(color, alpha);
}

/** The counterpart of glsl/f_oit_particles.glsl. */
[shader("fragment")]
OITOutput oitParticleFragment(Varyings input) {
    float4 shaded = shadeParticle(input);
    return packOIT(shaded.rgb, shaded.a, input.position.z);
}

[shader("fragment")]
float4 particleFragment(Varyings input) {
    return shadeParticle(input);
}
