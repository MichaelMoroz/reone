#include "u_aabb.glsl"
#include "u_globals.glsl"

layout(location = 0) in vec3 aPosition;

out vec4 fragPosWorld;
out vec4 fragCurClipPos;
out vec4 fragPrevClipPos;

void main() {
    fragPosWorld = uCorners[gl_VertexID];

    // Debug geometry, rebuilt every frame from world-space corners, so only the
    // camera contributes motion.
    fragCurClipPos = uViewProjection * fragPosWorld;
    fragPrevClipPos = uPrevViewProjection * fragPosWorld;

    gl_Position = uProjection * uView * fragPosWorld;
}
