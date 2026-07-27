#include "i_math.glsl"
#include "u_locals.glsl"

in vec4 fragPosWorld;

out vec4 fragColor;

uniform samplerCube sEnvMapCube;
uniform sampler2D sEnvMap;

vec3 sampleEnvironment(vec3 direction) {
    if (isFeatureEnabled(FEATURE_ENVMAPCUBE)) {
        return texture(sEnvMapCube, direction).rgb;
    }
    vec3 d = normalize(-direction);
    vec2 uv = vec2(0.5 + atan(d.x, d.z) / (2.0 * PI), 0.5 - asin(d.y) / PI);
    return texture(sEnvMap, uv).rgb;
}

void main() {
    vec3 N = normalize(fragPosWorld.xyz);

    vec3 irradiance = vec3(0.0);

    // tangent space calculation from origin point
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    float sampleDelta = 0.025;
    float nrSamples = 0.0f;
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            // spherical to cartesian (in tangent space)
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            // tangent space to world
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;

            irradiance += sampleEnvironment(sampleVec) * cos(theta) * sin(theta);
            nrSamples++;
        }
    }
    irradiance = PI * irradiance * (1.0 / float(nrSamples));

    fragColor = vec4(irradiance, 1.0);
}
