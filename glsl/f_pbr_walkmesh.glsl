#include "u_globals.glsl"
#include "u_walkmesh.glsl"

#include "i_motion.glsl"

in vec4 fragPosWorld;
in vec3 fragNormalWorld;
flat in int fragMaterial;
in vec4 fragCurClipPos;
in vec4 fragPrevClipPos;

layout(location = 0) out vec4 fragDiffuseColor;
layout(location = 1) out vec4 fragEyeNormal;
layout(location = 2) out vec4 fragLightmapColor;
layout(location = 3) out vec4 fragSelfIllumColor;
layout(location = 4) out vec2 fragMotion;

void main() {
    vec3 eyeNormal = transpose(mat3(uViewInv)) * normalize(fragNormalWorld);
    eyeNormal = 0.5 * eyeNormal + 0.5;

    fragDiffuseColor = vec4(uWalkmeshMaterials[fragMaterial].rgb, 1.0);
    fragLightmapColor = vec4(vec3(1.0), 0.0);
    fragSelfIllumColor = vec4(0.0);
    fragEyeNormal = vec4(eyeNormal, 0.0);
    fragMotion = computeMotion(fragCurClipPos, fragPrevClipPos);
}
