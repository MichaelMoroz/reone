// Reflection-only entry point for the C++ scene-table ABI guard.  Do not add
// it to a render pipeline or shader pack.
import lib.scene_schema;

[[vk::binding(0, 0)]] StructuredBuffer<InstanceMaterial> materials;
[[vk::binding(1, 0)]] StructuredBuffer<Matrix3x4> matrices;
[[vk::binding(2, 0)]] StructuredBuffer<MergedVertex> vertices;
[[vk::binding(3, 0)]] StructuredBuffer<SceneObject> objects;
[[vk::binding(4, 0)]] StructuredBuffer<ProceduralQuad> quads;
[[vk::binding(5, 0)]] StructuredBuffer<GrassFace> faces;
[[vk::binding(6, 0)]] StructuredBuffer<GrassRange> ranges;

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {}
