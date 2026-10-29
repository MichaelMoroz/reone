/*
 * Copyright (c) 2020-2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * The walkmesh and trigger debug view: one flat colour per surface material.
 *
 * Filled and depth-tested rather than drawn through the debug overlay, because
 * this has to read the same as the original's view - a solid sheet that the
 * room occludes - and the overlay's rule is to dim what is behind geometry
 * instead of hiding it.
 */

import uniforms;

struct WalkmeshVertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(9)]] float material : TEXCOORD5;
};

struct WalkmeshVaryings {
    float4 position : SV_Position;
    /** Flat: a face's surface id must not blend across its own triangle. */
    nointerpolation uint material : TEXCOORD0;
};

[shader("vertex")]
WalkmeshVaryings walkmeshVertex(WalkmeshVertexInput input) {
    WalkmeshVaryings output;
    float4 worldPosition = mul(localUniforms.model, float4(input.position, 1.0));
    output.position = mul(globalUniforms.projection,
                          mul(globalUniforms.view, worldPosition));
    // The attribute is normalised, so scale back to a slot index. Trigger
    // geometry rides in at a flat 1.0, which lands on the last slot.
    float slot = round(saturate(input.material) * (float) (kMaxWalkmeshMaterials - 1));
    output.material = (uint) slot;
    return output;
}

[shader("fragment")]
float4 walkmeshFragment(WalkmeshVaryings input) : SV_Target0 {
    return walkmeshUniforms.materials[input.material];
}
