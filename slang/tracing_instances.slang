import lib.scene_schema;

[[vk::binding(0, 1)]] StructuredBuffer<GrassCardInstance> grassCardInstances;
[[vk::binding(1, 1)]] RWByteAddressBuffer tracingInstances;
[[vk::binding(2, 1)]] RWStructuredBuffer<uint> variantCounts;

struct PushConstants {
    uint cardCount;
    uint cardRegionCapacity;
    uint instanceCount;
    uint clearRecords;
};

[[vk::push_constant]] PushConstants pushConstants;

static const uint kInstanceStride = 64;
static const uint kCustomIndexMaskOffset = 48;
static const uint kSbtOffsetFlagsOffset = 52;

void storeRows(uint offset, float4 row0, float4 row1, float4 row2) {
    tracingInstances.Store4(offset, asuint(row0));
    tracingInstances.Store4(offset + 16, asuint(row1));
    tracingInstances.Store4(offset + 32, asuint(row2));
}

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID) {
    if (pushConstants.clearRecords != 0) {
        if (id < 4)
            variantCounts[id] = 0;
        if (id >= pushConstants.instanceCount)
            return;

        uint offset = id * kInstanceStride;
        storeRows(offset,
                  float4(1.0, 0.0, 0.0, 0.0),
                  float4(0.0, 1.0, 0.0, 0.0),
                  float4(0.0, 0.0, 1.0, 0.0));
        tracingInstances.Store(offset + kCustomIndexMaskOffset,
                               id == 0 ? (kPtInstanceGeometry << 24) : 0u);
        tracingInstances.Store(offset + kSbtOffsetFlagsOffset, 0x01000000u);
        return;
    }

    if (id >= pushConstants.cardCount)
        return;
    GrassCardInstance card = grassCardInstances[id];
    uint variant = min(uint(card.lightmapVariantCulled.z), 3u);
    uint regionIndex;
    InterlockedAdd(variantCounts[variant], 1, regionIndex);
    if (regionIndex >= pushConstants.cardRegionCapacity)
        return;

    uint slot = 1 + variant * pushConstants.cardRegionCapacity + regionIndex;
    uint offset = slot * kInstanceStride;
    float3 width = float3(card.row0.x, card.row1.x, card.row2.x);
    float3 up = float3(card.row0.y, card.row1.y, card.row2.y);
    float3 normal = cross(width, up);
    float normalLengthSquared = dot(normal, normal);
    bool active = card.lightmapVariantCulled.w == 0.0 && normalLengthSquared > 1.0e-10;
    float4 row0 = float4(1.0, 0.0, 0.0, 0.0);
    float4 row1 = float4(0.0, 1.0, 0.0, 0.0);
    float4 row2 = float4(0.0, 0.0, 1.0, 0.0);
    if (active) {
        // The template lies at local z=0; the instance transform still has to
        // be invertible, and this third basis leaves every vertex unchanged.
        normal *= rsqrt(normalLengthSquared);
        row0 = float4(card.row0.xy, normal.x, card.row0.w);
        row1 = float4(card.row1.xy, normal.y, card.row1.w);
        row2 = float4(card.row2.xy, normal.z, card.row2.w);
    }
    storeRows(offset, row0, row1, row2);
    // Grass has its own mask bit so a ray can drop it in hardware; a culled card keeps 0.
    uint mask = active ? (kPtInstanceGrass << 24) : 0u;
    tracingInstances.Store(offset + kCustomIndexMaskOffset,
                           ((id + 1) & 0x00ffffffu) | mask);
    tracingInstances.Store(offset + kSbtOffsetFlagsOffset, 0x01000000u);
}
