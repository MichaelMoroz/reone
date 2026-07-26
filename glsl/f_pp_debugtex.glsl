#include "u_screeneffect.glsl"

#include "i_math.glsl"

/**
 * Renders a render target in a form the eye can read. Several targets hold
 * values that are not directly viewable: depth is non-linear and clusters near
 * 1.0, eye normals are biased into unit range, and motion vectors are small
 * signed offsets that would otherwise show as black.
 */

const int DEBUG_MODE_COLOR = 0;
const int DEBUG_MODE_DEPTH = 1;
const int DEBUG_MODE_EYE_NORMAL = 2;
const int DEBUG_MODE_MOTION = 3;
const int DEBUG_MODE_MOTION_MAGNITUDE = 4;

uniform sampler2D sMainTex;

uniform int uDebugMode;
uniform float uDebugScale;

noperspective in vec2 fragUV1;

out vec4 fragColor;

vec3 motionToColor(vec2 motion) {
    // Red is rightwards, green is upwards, grey is stationary. Signed values are
    // biased about 0.5 so that both directions remain visible.
    return vec3(0.5 + uDebugScale * motion, 0.5);
}

vec3 motionMagnitudeToColor(vec2 motion) {
    // Direction as hue, magnitude as intensity - the conventional optical flow
    // wheel, easier to read than the biased view when motion is small.
    float magnitude = clamp(uDebugScale * length(motion), 0.0, 1.0);
    float angle = atan(motion.y, motion.x);
    float hue = (angle + PI) / (2.0 * PI);
    vec3 k = mod(vec3(5.0, 3.0, 1.0) + 6.0 * hue, 6.0);
    vec3 rgb = 1.0 - max(min(min(k, 4.0 - k), 1.0), 0.0);
    return magnitude * rgb;
}

void main() {
    vec4 texel = texture(sMainTex, fragUV1);

    vec3 color;
    if (uDebugMode == DEBUG_MODE_DEPTH) {
        // Back to view-space distance, then normalised over the frustum.
        float ndc = 2.0 * texel.r - 1.0;
        float viewZ = 2.0 * uClipNear * uClipFar / (uClipFar + uClipNear - ndc * (uClipFar - uClipNear));
        color = vec3(uDebugScale * viewZ / uClipFar);
    } else if (uDebugMode == DEBUG_MODE_EYE_NORMAL) {
        color = normalize(2.0 * texel.rgb - 1.0) * 0.5 + 0.5;
    } else if (uDebugMode == DEBUG_MODE_MOTION) {
        color = motionToColor(texel.rg);
    } else if (uDebugMode == DEBUG_MODE_MOTION_MAGNITUDE) {
        color = motionMagnitudeToColor(texel.rg);
    } else {
        color = uDebugScale * texel.rgb;
    }

    fragColor = vec4(color, 1.0);
}
