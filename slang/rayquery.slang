// Primary-ray path tracer for Vulkan ray query.  Every hit reads its material
// and vertex attributes from GPU-resident data; no raster draw state is used.
import uniforms;
import lib.math;
import lib.surface;

[[vk::binding(0, 1)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] RaytracingAccelerationStructure sceneTLAS;

static const uint kNoTexture = 0xffffffffu;
static const float kGamma = 2.2;

struct InstanceMaterial {
    float4 selfIllumColor;
    float4 diffuseColor;
    float4 uv0;
    float4 uv1;
    float4 uv2;
    uint64_t vertexAddress;
    uint64_t indexAddress;
    uint vertexStride;
    int offPosition;
    int offNormals;
    int offUV1;
    int offUV2;
    int offTanSpace;
    uint mainTex;
    uint normalMap;
    uint lightmap;
    uint bumpMapArray;
    uint featureMask;
    int bumpMapFrame;
    float bumpMapScale;
};
[[vk::binding(2, 1)]] StructuredBuffer<InstanceMaterial> instanceMaterials;
[[vk::binding(3, 1)]] RWStructuredBuffer<uint> traceStats;
// This binding is deliberately both runtime-sized and non-uniformly indexed.
// The descriptor layout supplies UPDATE_AFTER_BIND and PARTIALLY_BOUND.
[[vk::binding(4, 1)]] Sampler2D bindlessTextures[];
// Animated grayscale bump maps are Texture2DArray assets. They cannot share
// bindlessTextures: a Sampler2D descriptor has a different Vulkan view type.
[[vk::binding(5, 1)]] Sampler2DArray bindlessTextureArrays[];

// Set CPU-side on fully self-illuminated instances; see rayquery.cpp.
static const uint kTraceSky = 1u << 24;
// Additive-blended diffuse: the texture is the light. Set CPU-side.
static const uint kTraceAdditive = 1u << 25;
// Punch-through diffuse uses deterministic multi-layer transparency in
// ray-query candidates.
static const uint kTracePunchThrough = 1u << 26;

struct PushConstants {
    uint frameIndex;
    uint samplesPerPixel;
    // Source intensities, not an image exposure: sky, other emitters, and the
    // baked radiance cache each get their own dial so the traced light balance
    // can be tuned live against raster without a shader rebuild.
    float skyIntensity;
    float emissiveIntensity;
    float lightmapIntensity;
    float directIntensity;
    // World-space offset of a secondary ray's origin along the geometric
    // normal. Too small self-intersects and speckles black; too large leaks
    // light through contact edges. A dial because the right value is
    // scene-scale-dependent and staring at acne beats recompiling.
    float rayOriginOffset;
    // World ambient, applied scaled by how little sky the pixel's hemisphere
    // saw - a stand-in for exactly the light the tracer could not find.
    float worldAmbientIntensity;
    float sunIntensity;
    // Bit 0 enables the traceStats counters. Off by default: even batched,
    // the atomics contend on one cache line from every thread in flight,
    // and the per-sample bounce counter fired millions of times per frame.
    uint traceFlags;
};
static const uint kTraceFlagStats = 1u;
[[vk::push_constant]] PushConstants pushConstants;


uint hash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float random01(inout uint state) {
    state = hash(state);
    return float(state & 0x00ffffffu) / 16777216.0;
}

float loadFloat(uint64_t address) {
    // This Slang version exposes physical storage through ConstBufferPointer
    // rather than DXC's vk::RawBufferLoad. The explicit 4-byte alignment is
    // the same raw-address operation, one component at a time.
    ConstBufferPointer<uint, 4> pointer = ConstBufferPointer<uint, 4>(address);
    return asfloat(pointer[0]);
}

float2 loadFloat2(uint64_t address) {
    return float2(loadFloat(address), loadFloat(address + 4));
}

float3 loadFloat3(uint64_t address) {
    return float3(loadFloat(address), loadFloat(address + 4), loadFloat(address + 8));
}

uint loadIndex16(uint64_t indexAddress, uint index) {
    uint64_t address = indexAddress + uint64_t(index) * 2;
    ConstBufferPointer<uint, 4> pointer = ConstBufferPointer<uint, 4>(address & ~uint64_t(3));
    uint packed = pointer[0];
    return (packed >> (uint(address & 2) * 8)) & 0xffffu;
}

uint3 triangleIndices(InstanceMaterial material, uint primitive) {
    uint first = primitive * 3;
    return uint3(loadIndex16(material.indexAddress, first),
                 loadIndex16(material.indexAddress, first + 1),
                 loadIndex16(material.indexAddress, first + 2));
}

uint64_t attributeAddress(InstanceMaterial material, uint vertex, int offset) {
    return material.vertexAddress + uint64_t(vertex) * uint64_t(material.vertexStride) + uint64_t(offset);
}

float2 interpolateFloat2(InstanceMaterial material, uint3 indices, int offset, float3 weights) {
    return loadFloat2(attributeAddress(material, indices.x, offset)) * weights.x +
           loadFloat2(attributeAddress(material, indices.y, offset)) * weights.y +
           loadFloat2(attributeAddress(material, indices.z, offset)) * weights.z;
}

float3 interpolateFloat3(InstanceMaterial material, uint3 indices, int offset, float3 weights) {
    return loadFloat3(attributeAddress(material, indices.x, offset)) * weights.x +
           loadFloat3(attributeAddress(material, indices.y, offset)) * weights.y +
           loadFloat3(attributeAddress(material, indices.z, offset)) * weights.z;
}

float2 transformedUV(InstanceMaterial material, float2 uv) {
    float3 h = float3(uv, 1.0);
    return float2(material.uv0.x * h.x + material.uv1.x * h.y + material.uv2.x * h.z,
                  material.uv0.y * h.x + material.uv1.y * h.y + material.uv2.y * h.z);
}

float3 cosineHemisphere(float3 normal, float2 xi) {
    float radius = sqrt(xi.x);
    float phi = 6.28318530718 * xi.y;
    float3 local = float3(radius * cos(phi), radius * sin(phi), sqrt(1.0 - xi.x));
    float3 reference = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 tangent = normalize(cross(reference, normal));
    float3 bitangent = cross(normal, tangent);
    return tangent * local.x + bitangent * local.y + normal * local.z;
}

float3 tangentToWorld(float3 normal, float3 local) {
    float3 reference = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    float3 tangent = normalize(cross(reference, normal));
    float3 bitangent = cross(normal, tangent);
    return tangent * local.x + bitangent * local.y + normal * local.z;
}

// Importance sample the GGX normal-distribution function. a is alpha in the
// raster resolve's notation (roughness squared), not perceptual roughness.
float3 ggxHalfVector(float3 normal, float roughness, float2 xi) {
    float a = roughness * roughness;
    float a2 = a * a;
    float phi = 6.28318530718 * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / max(1e-6, 1.0 + (a2 - 1.0) * xi.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    return tangentToWorld(normal, float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}

float3 fresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 linearToGamma(float3 rgb) {
    return pow(max(rgb, 0.0), 1.0 / 2.2);
}

float3 gammaToLinear(float3 rgb) {
    return pow(rgb, float3(kGamma));
}

float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    float3 ceiling = max(float3(1.0 - roughness), F0);
    return F0 + (ceiling - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float distributionGGX(float NdotH2, float a2) {
    float d = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

float geometrySchlick(float NdotV, float k) {
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(float NdotL, float NdotV, float k) {
    return geometrySchlick(NdotL, k) * geometrySchlick(NdotV, k);
}

float luminance(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}

struct CandidateLayer {
    float3 albedo;
    float alpha;
    float3 radiance;
};

// Candidate handling deliberately stays cheap: its only job is to composite a
// translucent leaf layer while traversal continues to a solid surface. It does
// not launch another hemisphere from every candidate.
CandidateLayer sampleCandidateLayer(InstanceMaterial material, uint3 indices, float3 weights) {
    CandidateLayer layer;
    layer.albedo = gammaToLinear(material.diffuseColor.rgb);
    layer.alpha = 1.0;
    float2 uv = float2(0.0);
    if (material.offUV1 != -1) {
        uv = transformedUV(material, interpolateFloat2(material, indices, material.offUV1, weights));
    }
    if (material.mainTex != kNoTexture) {
        float4 sample = bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleLevel(uv, 0.0);
        layer.albedo = gammaToLinear(sample.rgb);
        layer.alpha = sample.a;
    }
    float emissionScale = (material.featureMask & kTraceSky) != 0
                              ? pushConstants.skyIntensity
                              : pushConstants.emissiveIntensity;
    layer.radiance = material.selfIllumColor.rgb * layer.albedo * emissionScale;
    if ((material.featureMask & uint(kFeatureLightmap)) != 0 &&
        material.lightmap != kNoTexture && material.offUV2 != -1) {
        float2 uv2 = interpolateFloat2(material, indices, material.offUV2, weights);
        float3 lightmap = bindlessTextures[NonUniformResourceIndex(material.lightmap)].SampleLevel(uv2, 0.0).rgb;
        layer.radiance += layer.albedo * lightmap * pushConstants.lightmapIntensity;
    }
    return layer;
}

float3 additiveRadiance(InstanceMaterial material, CandidateLayer layer) {
    // Additive diffuse textures are authored as light even when no self-illum
    // controller is present. This is the same emission*albedo convention used
    // by sampleSurface for kTraceAdditive.
    return max(material.selfIllumColor.rgb, float3(1.0)) * layer.albedo *
           pushConstants.emissiveIntensity;
}

struct HitSurface {
    SurfaceParams surface;
    float3 geometricNormal;
};

HitSurface sampleSurface(InstanceMaterial material, uint3 indices, float3 weights,
                         float3x4 objectToWorld, float3x4 worldToObject,
                         float3 rayDirection, float2 uv, float2 uv2) {
    HitSurface hit;
    float3 p0 = mul(objectToWorld, float4(loadFloat3(attributeAddress(material, indices.x, material.offPosition)), 1.0));
    float3 p1 = mul(objectToWorld, float4(loadFloat3(attributeAddress(material, indices.y, material.offPosition)), 1.0));
    float3 p2 = mul(objectToWorld, float4(loadFloat3(attributeAddress(material, indices.z, material.offPosition)), 1.0));
    hit.geometricNormal = normalize(cross(p1 - p0, p2 - p0));
    if (dot(hit.geometricNormal, rayDirection) > 0.0) hit.geometricNormal = -hit.geometricNormal;

    SurfaceParams surface = defaultSurface();
    if (material.mainTex != kNoTexture) {
        float4 mainTexSample = bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleLevel(uv, 0.0);
        // Textures are stored gamma-encoded; every term this shader computes is
        // linear, exactly as the raster resolve does it. Missing this halved
        // every light-driven (dark) region: linear 0.1 displayed raw is 25 of
        // 255 where the encoded value is 90.
        surface.albedo = gammaToLinear(mainTexSample.rgb);
        surface.alpha = mainTexSample.a;
    }
    // Keep the tracer's stand-ins exactly aligned with pbr_resolve.slang:
    // alpha is only a reflectance control on env-mapped materials, never
    // metalness, and roughness still uses alpha for every material.
    surface.metallic = 0.0;
    surface.roughness = clamp(surface.alpha, 0.2, 1.0);

    float3 shadingNormal = hit.geometricNormal;
    if (material.offNormals != -1) {
        shadingNormal = normalize(interpolateFloat3(material, indices, material.offNormals, weights));
    }
    if (material.offTanSpace != -1) {
        // offTanSpace is bitangent, tangent, tangent-space normal in that order.
        float3 interpolatedBitangent = interpolateFloat3(material, indices, material.offTanSpace, weights);
        float3 tangent = interpolateFloat3(material, indices, material.offTanSpace + 12, weights);
        float3 tangentSpaceNormal = interpolateFloat3(material, indices, material.offTanSpace + 24, weights);
        tangent = normalize(tangent - shadingNormal * dot(tangent, shadingNormal));
        float handedness = dot(cross(shadingNormal, tangent), interpolatedBitangent) < 0.0 ? -1.0 : 1.0;
        float3 bitangent = cross(shadingNormal, tangent) * handedness;
        // The third vector is normally the smooth normal. It also guards corrupt
        // tangent frames before either normal-map variant samples them.
        if (dot(tangentSpaceNormal, tangentSpaceNormal) >= 1e-10 &&
            dot(tangent, tangent) >= 1e-10 && dot(bitangent, bitangent) >= 1e-10) {
            if ((material.featureMask & uint(kFeatureNormalMap)) != 0 && material.normalMap != kNoTexture) {
                float3 n = bindlessTextures[NonUniformResourceIndex(material.normalMap)].SampleLevel(uv, 0.0).rgb * 2.0 - 1.0;
                shadingNormal = normalize(tangent * n.x + bitangent * n.y + shadingNormal * n.z);
            } else if ((material.featureMask & uint(kFeatureBumpMap)) != 0 && material.bumpMapArray != kNoTexture) {
                uint bumpWidth, bumpHeight, bumpLayers;
                bindlessTextureArrays[NonUniformResourceIndex(material.bumpMapArray)].GetDimensions(
                    bumpWidth, bumpHeight, bumpLayers);
                float2 texel = 1.0 / float2(max(bumpWidth, 1u), max(bumpHeight, 1u));
                float layer = float(material.bumpMapFrame);
                float height = bindlessTextureArrays[NonUniformResourceIndex(material.bumpMapArray)].SampleLevel(float3(uv, layer), 0.0).r;
                // Compute dispatches have no screen-space ddx/ddy. These explicit
                // texel differences are an approximation of the raster bump path.
                float dX = material.bumpMapScale * (bindlessTextureArrays[NonUniformResourceIndex(material.bumpMapArray)].SampleLevel(float3(uv + float2(texel.x, 0.0), layer), 0.0).r - height);
                float dY = material.bumpMapScale * (bindlessTextureArrays[NonUniformResourceIndex(material.bumpMapArray)].SampleLevel(float3(uv + float2(0.0, texel.y), layer), 0.0).r - height);
                float3 n = normalize(float3(-dX, -dY, 1.0));
                shadingNormal = normalize(tangent * n.x + bitangent * n.y + shadingNormal * n.z);
            }
        }
    }
    // Attributes are object-space; this row-vector product is the inverse
    // transpose. SurfaceParams always carries a world-space normal.
    surface.normal = normalize(mul(shadingNormal, (float3x3)worldToObject));
    if (dot(surface.normal, rayDirection) > 0.0) surface.normal = -surface.normal;
    if ((material.featureMask & uint(kFeatureLightmap)) != 0) {
        surface.hasLightmap = true;
        if (material.lightmap != kNoTexture && material.offUV2 != -1) {
            surface.lightmap = gammaToLinear(bindlessTextures[NonUniformResourceIndex(material.lightmap)].SampleLevel(uv2, 0.0).rgb);
        }
    }
    surface.emissive = gammaToLinear(material.selfIllumColor.rgb);
    if ((material.featureMask & kTraceAdditive) != 0) {
        // Emission of 1 makes the additive texture read at its own brightness
        // through the emission*albedo path, matching how raster composites it.
        surface.emissive = max(surface.emissive, float3(1.0));
    }
    hit.surface = surface;
    return hit;
}

// A shadow query visits all punch-through candidates in one traversal. Their
// (1 - alpha) product is order-independent, unlike the primary ray's visible
// layer compositing, so it needs neither sorting nor restart machinery.
float shadowTransmittance(float3 hitPosition, float3 geometricNormal, float3 direction,
                          float lightDistance) {
    static const float kShadowEpsilon = 1e-3;
    RayDesc shadowRay;
    if (lightDistance <= kShadowEpsilon) return 1.0;
    shadowRay.Origin = hitPosition + geometricNormal * pushConstants.rayOriginOffset;
    shadowRay.Direction = direction;
    shadowRay.TMin = kShadowEpsilon;
    shadowRay.TMax = max(0.0, lightDistance - kShadowEpsilon);

    RayQuery<RAY_FLAG_NONE> shadow;
    // Mask bit 2 is the sky dome. It must stay visible to camera and bounce
    // rays (they shade it as the environment) but must never occlude a
    // shadow ray: the promoted sun sits beyond the dome, and with the full
    // mask every sun ray committed on it and the sun lit nothing at all.
    shadow.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0x1, shadowRay);
    float transmittance = 1.0;
    while (shadow.Proceed()) {
        if (shadow.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        InstanceMaterial candidateMaterial = instanceMaterials[shadow.CandidateInstanceIndex()];
        if ((candidateMaterial.featureMask & kTraceAdditive) != 0) {
            // Additive surfaces are emitters, not occluders.
            continue;
        }
        if ((candidateMaterial.featureMask & kTracePunchThrough) != 0) {
            uint3 candidateIndices = triangleIndices(candidateMaterial,
                                                     shadow.CandidatePrimitiveIndex());
            float2 barycentrics = shadow.CandidateTriangleBarycentrics();
            float3 weights = float3(1.0 - barycentrics.x - barycentrics.y,
                                    barycentrics.x, barycentrics.y);
            CandidateLayer layer = sampleCandidateLayer(candidateMaterial, candidateIndices, weights);
            transmittance = clamp(transmittance * (1.0 - layer.alpha), 0.0, 1.0);
            if (transmittance < kShadowEpsilon) break;
        }
    }
    // Opaque geometry commits automatically. A generic query is required here
    // because punch-through candidates must stay visible to the loop above;
    // ACCEPT_FIRST_HIT is only valid for the opaque-only fast path.
    return shadow.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : transmittance;
}

// The direct-light selection weight: receiver-dependent importance, with
// attenuation pinned to 1 for promoted directionals so distance never erases
// the sun. Zero for lights the sampler never draws - past raster's
// radius-squared cutoff quirk, or ambient-only points (those feed the flat
// irradiance pre-pass instead). Both the totalling walk and the multiplicity
// pass call this so their float accumulation is bit-identical; that identity
// is what lets the weights be recomputed instead of stored in a scratch
// array.
float directLightWeight(GlobalUniformsLight light, float3 hitPosition, float3 shadingNormal) {
    bool directional = light.position.w == 0.0;
    float3 fragToLight = light.position.xyz - hitPosition;
    float lightDistance = length(fragToLight);
    if (!directional && lightDistance > light.radius * light.radius) return 0.0;
    if (!directional && light.ambientOnly != 0) return 0.0;
    float attenuation = 1.0;
    if (!directional) {
        float d = light.radius + lightDistance;
        attenuation = (light.radius * light.radius) / (d * d);
    }
    float NdotL = max(0.0, dot(shadingNormal, fragToLight / max(lightDistance, 1e-4)));
    return max(0.0, luminance(gammaToLinear(light.color.rgb)) *
                      light.multiplier * attenuation * NdotL);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint width, height;
    outputImage.GetDimensions(width, height);
    if (dispatchId.x >= width || dispatchId.y >= height) return;

    float2 ndc = ((float2(dispatchId.xy) + 0.5) / float2(width, height)) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 eye = mul(globalUniforms.projectionInv, float4(ndc, 1.0, 1.0));
    eye /= eye.w;
    RayDesc ray;
    ray.Origin = globalUniforms.cameraPosition.xyz;
    ray.Direction = normalize(mul(globalUniforms.viewInv, float4(eye.xyz, 0.0)).xyz);
    ray.TMin = 0.01;
    ray.TMax = 100000.0;

    // Transparency composites strictly nearest-first. Ray-query candidates
    // arrive in BVH traversal order, not distance order, so compositing inside
    // one Proceed loop layers surfaces in whatever order the tree yields -
    // wrong whenever two transparent surfaces overlap. Each segment instead
    // commits the NEAREST surface with meaningful coverage, composites exactly
    // that one, and re-traces with TMin advanced just past it. The origin
    // never moves, so CommittedRayT stays in the original parameterisation and
    // the shading block below needs no special cases.
    RayQuery<RAY_FLAG_NONE> primary;
    float primaryThroughput = 1.0;
    float3 primaryLayers = 0.0;
    static const uint kMaxTransparentLayers = 8;
    static const uint kMaxAdditiveHits = 8;
    for (uint segment = 0; segment < kMaxTransparentLayers; ++segment) {
        float4 primaryAdditiveHits[kMaxAdditiveHits];
        uint primaryAdditiveCount = 0;
        primary.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
        while (primary.Proceed()) {
            if (primary.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
            uint candidateInstance = primary.CandidateInstanceIndex();
            InstanceMaterial candidateMaterial = instanceMaterials[candidateInstance];
            uint3 candidateIndices = triangleIndices(candidateMaterial, primary.CandidatePrimitiveIndex());
            float2 candidateBarycentrics = primary.CandidateTriangleBarycentrics();
            float3 candidateWeights = float3(1.0 - candidateBarycentrics.x - candidateBarycentrics.y,
                                              candidateBarycentrics.x, candidateBarycentrics.y);
            CandidateLayer layer = sampleCandidateLayer(candidateMaterial, candidateIndices, candidateWeights);
            if ((candidateMaterial.featureMask & kTraceAdditive) != 0) {
                // Candidates surface in traversal order, and nothing stops one
                // arriving beyond what later becomes the nearest hit - or
                // being re-surfaced by the next restart segment, since TMin
                // only ever advances past committed layers. Paying additive
                // light immediately therefore double-counted every blade
                // behind a leaf and leaked glow through walls. Record them
                // with their distance instead; they are settled against the
                // segment's committed interval below, which each additive
                // surface falls into exactly once.
                // Kept nearest-first by insertion, not first-come: candidate
                // arrival order is whatever the driver's BVH build produced,
                // and builds are not run-reproducible - a first-come cap made
                // overflow pixels nondeterministic between identical runs.
                float4 additiveEntry = float4(additiveRadiance(candidateMaterial, layer),
                                              primary.CandidateTriangleRayT());
                if (primaryAdditiveCount < kMaxAdditiveHits ||
                    additiveEntry.w < primaryAdditiveHits[kMaxAdditiveHits - 1].w) {
                    uint insertAt = min(primaryAdditiveCount, kMaxAdditiveHits - 1);
                    while (insertAt > 0 && primaryAdditiveHits[insertAt - 1].w > additiveEntry.w) {
                        primaryAdditiveHits[insertAt] = primaryAdditiveHits[insertAt - 1];
                        --insertAt;
                    }
                    primaryAdditiveHits[insertAt] = additiveEntry;
                    primaryAdditiveCount = min(primaryAdditiveCount + 1, kMaxAdditiveHits);
                }
            } else if ((candidateMaterial.featureMask & kTracePunchThrough) != 0) {
                // Commit anything with meaningful coverage; the query keeps
                // the nearest. Fully transparent texels never commit and cost
                // nothing further.
                if (layer.alpha > 0.01) {
                    primary.CommitNonOpaqueTriangleHit();
                }
            }
        }
        // Settle this segment's additive candidates: only those in front of
        // the committed hit are visible, and the interval [TMin, committedT)
        // contains no transparent layers - the committed hit is the nearest -
        // so segment-entry throughput is exact for all of them.
        float committedT = primary.CommittedStatus() == COMMITTED_TRIANGLE_HIT
                               ? primary.CommittedRayT()
                               : 1e30;
        for (uint i = 0; i < primaryAdditiveCount; ++i) {
            if (primaryAdditiveHits[i].w < committedT) {
                primaryLayers += primaryThroughput * primaryAdditiveHits[i].rgb;
            }
        }
        if (primary.CommittedStatus() != COMMITTED_TRIANGLE_HIT) break;
        InstanceMaterial committedMaterial = instanceMaterials[primary.CommittedInstanceIndex()];
        if ((committedMaterial.featureMask & kTracePunchThrough) == 0) break;
        uint3 committedIndices = triangleIndices(committedMaterial, primary.CommittedPrimitiveIndex());
        float2 committedBarycentrics = primary.CommittedTriangleBarycentrics();
        float3 committedWeights = float3(1.0 - committedBarycentrics.x - committedBarycentrics.y,
                                          committedBarycentrics.x, committedBarycentrics.y);
        CandidateLayer committedLayer = sampleCandidateLayer(committedMaterial, committedIndices, committedWeights);
        // An effectively opaque texel is a real surface and deserves the full
        // shading path rather than the cheap layer formula.
        if (committedLayer.alpha >= 0.99) break;
        primaryLayers += primaryThroughput * committedLayer.alpha * committedLayer.radiance;
        primaryThroughput *= 1.0 - committedLayer.alpha;
        if (primaryThroughput < 1e-3) break;
        ray.TMin = primary.CommittedRayT() + 1e-3;
    }

    float3 color = primaryLayers;
    if (primary.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        uint primaryInstance = primary.CommittedInstanceIndex();
        InstanceMaterial material = instanceMaterials[primaryInstance];
        uint3 indices = triangleIndices(material, primary.CommittedPrimitiveIndex());
        float2 barycentrics = primary.CommittedTriangleBarycentrics();
        float3 weights = float3(1.0 - barycentrics.x - barycentrics.y,
                                barycentrics.x, barycentrics.y);
        float3x4 objectToWorld = primary.CommittedObjectToWorld3x4();
        float3x4 worldToObject = primary.CommittedWorldToObject3x4();
        float2 uv = float2(0.0);
        if (material.offUV1 != -1) {
            uv = transformedUV(material, interpolateFloat2(material, indices, material.offUV1, weights));
        }
        float2 uv2 = float2(0.0);
        if (material.offUV2 != -1) {
            uv2 = interpolateFloat2(material, indices, material.offUV2, weights);
        }
        HitSurface primarySurface = sampleSurface(material, indices, weights, objectToWorld,
                                                  worldToObject, ray.Direction, uv, uv2);
        float3 hitPosition = ray.Origin + ray.Direction * primary.CommittedRayT();
        // kTraceSky selects only the source-intensity dial. Like raster's
        // self-illumination, emission is additive; it never replaces shading.
        float3 diffuseIllumination = 0.0;
        float skySamples = 0.0;
        float3 specularIllumination = 0.0;
        float3 V = -ray.Direction;
        bool envmapped = (material.featureMask & uint(kFeatureEnvMap)) != 0;
        float envMapStrength = envmapped ? (1.0 - primarySurface.surface.alpha) : 0.0;
        float3 F0 = lerp(lerp(float3(0.04), primarySurface.surface.albedo,
                               primarySurface.surface.metallic),
                          float3(1.0), envMapStrength);
        float NdotV = max(0.0, dot(primarySurface.surface.normal, V));
        float3 F = fresnelSchlick(NdotV, F0);
        float a = primarySurface.surface.roughness * primarySurface.surface.roughness;
        float a2 = a * a;
        float k = primarySurface.surface.roughness + 1.0;
        k = k * k * (1.0 / 8.0);
        float3 irradiance = float3(0.0);
        uint survivingLights = 0;
        // One walk per pixel serves both light paths: ambient-only points
        // accumulate raster's cheap flat irradiance (deliberately kept from
        // the raster resolve), everything else totals a selection weight for
        // the multiplicity pass below. Directionals are promoted to sampled,
        // ray-shadowed suns even when authored ambient-only: raster has no
        // direct sun at all (only baked lightmaps plus cascade-map
        // darkening), so there is no parity target and the Sun dial grades
        // it. Weights are recomputed by the second pass instead of stored: a
        // dynamically indexed local array lives in scratch, not registers,
        // and removing it was worth 11 ms alongside batching what were then
        // per-ray stats atomics.
        float totalLightWeight = 0.0;
        for (int i = 0; i < globalUniforms.numLights; ++i) {
            GlobalUniformsLight light = globalUniforms.lights[i];
            bool directional = light.position.w == 0.0;
            float3 fragToLight = light.position.xyz - hitPosition;
            float lightDistance = length(fragToLight);
            // Kept exactly as the raster resolve has it: radius squared is
            // compared to un-squared distance, despite the unusual reach.
            if (!directional && lightDistance > light.radius * light.radius) continue;
            ++survivingLights;
            if (!directional && light.ambientOnly != 0) {
                float d = light.radius + lightDistance;
                float attenuation = (light.radius * light.radius) / (d * d);
                irradiance += light.multiplier * attenuation * gammaToLinear(light.color.rgb);
                continue;
            }
            totalLightWeight += directLightWeight(light, hitPosition, primarySurface.surface.normal);
        }
        bool statsOn = (pushConstants.traceFlags & kTraceFlagStats) != 0;
        if (statsOn) {
            InterlockedAdd(traceStats[2], survivingLights);
            InterlockedAdd(traceStats[3], 1);
        }
        float3 ambientF = fresnelSchlickRoughness(NdotV, F0, primarySurface.surface.roughness);
        float3 ambientD = (1.0 - ambientF) * (1.0 - primarySurface.surface.metallic) * irradiance;
        // Raster starts irradiance with worldAmbientColor. The tracer does not:
        // its sky hemisphere integral is that ambient outdoors, and adding both
        // double-counts it. The small missing flat interior term is intentional.
        float3 directIllumination = ambientD * primarySurface.surface.albedo;
        // The GGX lobe exists only where raster shows this heuristic a
        // specular response: environment-mapped surfaces. Everywhere else the
        // diffuse alpha is coverage or opacity, so the derived roughness is
        // meaningless - grass texels with soft alpha turned glossy and the
        // whole frame sparkled. Non-envmapped surfaces are pure diffuse.
        float specularProbability = envmapped ? clamp(luminance(F), 1e-3, 1.0 - 1e-3) : 0.0;
        uint sampleCount = max(pushConstants.samplesPerPixel, 1u);
        // Direct lighting stays at pixel scope, touches no scratch array,
        // and issues no unconditional atomics. Every sample still draws its
        // own light (the draw is recomputed per light interval from a
        // dedicated hash chain, one compare each), but a light picked by
        // several samples is shaded and shadow-traced once with the
        // multiplicity folded into the estimator, and the light fetch
        // happens at a warp-uniform index:
        // (1/spp) sum_s f(L_s)/p_s  =  sum_i (m_i/spp) f(L_i)/p_i.
        uint shadowRayCount = 0;
        if (totalLightWeight > 0.0) {
            float accumBefore = 0.0;
            for (int i = 0; i < globalUniforms.numLights; ++i) {
                GlobalUniformsLight light = globalUniforms.lights[i];
                float w = directLightWeight(light, hitPosition, primarySurface.surface.normal);
                if (w <= 0.0) continue;
                float accumAfter = accumBefore + w;
                // The last positive weight absorbs float rounding at the top
                // of the table, mirroring the closed lower end at zero.
                bool lastPositive = accumAfter >= totalLightWeight * (1.0 - 1e-6);
                uint multiplicity = 0;
                for (uint s = 0; s < sampleCount; ++s) {
                    uint pickState = hash(dispatchId.x * 0x9e3779b9u ^ dispatchId.y * 0x85ebca6bu ^
                                          s * 0xc2b2ae35u ^ pushConstants.frameIndex ^ 0x5bf03635u);
                    float pick = random01(pickState) * totalLightWeight;
                    if (pick >= accumBefore && (pick < accumAfter || lastPositive)) ++multiplicity;
                }
                accumBefore = accumAfter;
                if (multiplicity == 0) continue;
                bool directional = light.position.w == 0.0;
                float3 fragToLight = light.position.xyz - hitPosition;
                float lightDistance = length(fragToLight);
                float3 L = fragToLight / max(lightDistance, 1e-4);
                float attenuation = 1.0;
                if (!directional) {
                    float d = light.radius + lightDistance;
                    attenuation = (light.radius * light.radius) / (d * d);
                }
                float selectionProbability = w / totalLightWeight;
                float3 radiance = gammaToLinear(light.color.rgb) * light.multiplier;
                radiance *= directional ? pushConstants.sunIntensity
                                        : attenuation * pushConstants.directIntensity;
                // Counted locally: a per-ray InterlockedAdd on this shared
                // slot serializes every thread on one cache line.
                ++shadowRayCount;
                radiance *= shadowTransmittance(hitPosition, primarySurface.geometricNormal,
                                                L, directional ? 10000.0 : lightDistance) *
                            (float(multiplicity) / selectionProbability);
                float NdotL = max(0.0, dot(primarySurface.surface.normal, L));
                float3 H = normalize(V + L);
                float NdotH = max(0.0, dot(primarySurface.surface.normal, H));
                float VdotH = max(0.0, dot(V, H));
                float D = distributionGGX(NdotH * NdotH, a2);
                float G = geometrySmith(NdotL, NdotV, k);
                float3 lightF = fresnelSchlick(VdotH, F0);
                float3 spec = (D * G * lightF) / max(1e-4, 4.0 * NdotL * NdotV);
                float3 kD = (1.0 - lightF) * (1.0 - primarySurface.surface.metallic);
                diffuseIllumination += kD / kPi * radiance * NdotL * primarySurface.surface.albedo;
                // Raster's dynamic-light specular is meaningful only on the
                // same env-mapped surfaces that the tracer gives a specular
                // lobe.
                if (envmapped) specularIllumination += spec * radiance * NdotL;
            }
        }
        if (statsOn && shadowRayCount != 0) InterlockedAdd(traceStats[4], shadowRayCount);
            for (uint sample = 0; sample < sampleCount; ++sample) {
                uint state = hash(dispatchId.x * 0x9e3779b9u ^ dispatchId.y * 0x85ebca6bu ^
                                  sample * 0xc2b2ae35u ^ pushConstants.frameIndex);
                RayDesc secondary;
                secondary.Origin = hitPosition + primarySurface.geometricNormal * pushConstants.rayOriginOffset;
                bool sampleSpecular = envmapped && random01(state) < specularProbability;
                if (sampleSpecular) {
                    float3 H = ggxHalfVector(primarySurface.surface.normal,
                                              primarySurface.surface.roughness,
                                              float2(random01(state), random01(state)));
                    secondary.Direction = normalize(reflect(-V, H));
                } else {
                    secondary.Direction = cosineHemisphere(primarySurface.surface.normal,
                                                           float2(random01(state), random01(state)));
                }
                // A smooth normal can sample below the geometric surface at a
                // silhouette. Reflect that lobe into the geometric hemisphere
                // rather than immediately self-intersecting the same triangle.
                if (dot(secondary.Direction, primarySurface.geometricNormal) < 0.0) {
                    secondary.Direction = normalize(secondary.Direction -
                        2.0 * dot(secondary.Direction, primarySurface.geometricNormal) * primarySurface.geometricNormal);
                }
                secondary.TMin = 0.001;
                secondary.TMax = 100000.0;
                RayQuery<RAY_FLAG_NONE> bounce;
                bounce.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, secondary);
                float bounceThroughput = 1.0;
                float3 bounceLayers = 0.0;
                float4 bounceAdditiveHits[kMaxAdditiveHits];
                uint bounceAdditiveCount = 0;
                while (bounce.Proceed()) {
                    if (bounce.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
                    uint candidateInstance = bounce.CandidateInstanceIndex();
                    InstanceMaterial candidateMaterial = instanceMaterials[candidateInstance];
                    uint3 candidateIndices = triangleIndices(candidateMaterial, bounce.CandidatePrimitiveIndex());
                    float2 candidateBarycentrics = bounce.CandidateTriangleBarycentrics();
                    float3 candidateWeights = float3(1.0 - candidateBarycentrics.x - candidateBarycentrics.y,
                                                      candidateBarycentrics.x, candidateBarycentrics.y);
                    CandidateLayer layer = sampleCandidateLayer(candidateMaterial, candidateIndices, candidateWeights);
                    if ((candidateMaterial.featureMask & kTraceAdditive) != 0) {
                        // Nearest-first under the cap, same as the primary
                        // loop: arrival order is not run-reproducible.
                        float4 additiveEntry = float4(additiveRadiance(candidateMaterial, layer),
                                                      bounce.CandidateTriangleRayT());
                        if (bounceAdditiveCount < kMaxAdditiveHits ||
                            additiveEntry.w < bounceAdditiveHits[kMaxAdditiveHits - 1].w) {
                            uint insertAt = min(bounceAdditiveCount, kMaxAdditiveHits - 1);
                            while (insertAt > 0 && bounceAdditiveHits[insertAt - 1].w > additiveEntry.w) {
                                bounceAdditiveHits[insertAt] = bounceAdditiveHits[insertAt - 1];
                                --insertAt;
                            }
                            bounceAdditiveHits[insertAt] = additiveEntry;
                            bounceAdditiveCount = min(bounceAdditiveCount + 1, kMaxAdditiveHits);
                        }
                    } else if ((candidateMaterial.featureMask & kTracePunchThrough) != 0) {
                        if (layer.alpha >= 0.99) {
                            bounce.CommitNonOpaqueTriangleHit();
                        } else {
                            bounceLayers += bounceThroughput * layer.alpha * layer.radiance;
                            bounceThroughput *= 1.0 - layer.alpha;
                            if (bounceThroughput < 1e-3) break;
                        }
                    }
                }
                if (statsOn) InterlockedAdd(traceStats[0], 1);
                bool sampleSawSky = false;
                float bounceCommittedT = bounce.CommittedStatus() == COMMITTED_TRIANGLE_HIT
                                             ? bounce.CommittedRayT()
                                             : 1e30;
                for (uint i = 0; i < bounceAdditiveCount; ++i) {
                    if (bounceAdditiveHits[i].w < bounceCommittedT) {
                        bounceLayers += bounceThroughput * bounceAdditiveHits[i].rgb;
                    }
                }
                float3 radiance = bounceLayers;
                if (bounce.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                    InstanceMaterial bounceMaterial = instanceMaterials[bounce.CommittedInstanceIndex()];
                    uint3 bounceIndices = triangleIndices(bounceMaterial, bounce.CommittedPrimitiveIndex());
                    float2 bounceBarycentrics = bounce.CommittedTriangleBarycentrics();
                    float3 bounceWeights = float3(1.0 - bounceBarycentrics.x - bounceBarycentrics.y,
                                                   bounceBarycentrics.x, bounceBarycentrics.y);
                    float2 bounceUV = float2(0.0);
                    if (bounceMaterial.offUV1 != -1) {
                        bounceUV = transformedUV(bounceMaterial, interpolateFloat2(bounceMaterial, bounceIndices,
                                                                                     bounceMaterial.offUV1, bounceWeights));
                    }
                    float2 bounceUV2 = float2(0.0);
                    if (bounceMaterial.offUV2 != -1) {
                        bounceUV2 = interpolateFloat2(bounceMaterial, bounceIndices,
                                                      bounceMaterial.offUV2, bounceWeights);
                    }
                    HitSurface bounceSurface = sampleSurface(bounceMaterial, bounceIndices, bounceWeights,
                                                             bounce.CommittedObjectToWorld3x4(),
                                                             bounce.CommittedWorldToObject3x4(),
                                                             secondary.Direction, bounceUV, bounceUV2);
                    float bounceScale = (bounceMaterial.featureMask & kTraceSky) != 0
                                            ? pushConstants.skyIntensity
                                            : pushConstants.emissiveIntensity;
                    radiance += bounceThroughput * bounceSurface.surface.emissive *
                                 bounceSurface.surface.albedo * bounceScale;
                    if ((bounceMaterial.featureMask & kTraceSky) != 0) sampleSawSky = true;
                    if (bounceSurface.surface.hasLightmap) {
                        // A lightmap stores irradiance, not radiance: using it as
                        // an area-light cache loses directionality and is off by pi.
                        // Other lightmapped surfaces can therefore light this one
                        // again through its own traced hemisphere; that bounded
                        // double-count is preferable to unlit interiors.
                        radiance += bounceThroughput * bounceSurface.surface.albedo *
                                    bounceSurface.surface.lightmap * pushConstants.lightmapIntensity;
                    }
                    if (sampleSpecular) {
                        specularIllumination += radiance * F / specularProbability;
                    } else if (envmapped) {
                        diffuseIllumination += radiance * primarySurface.surface.albedo * (1.0 - F) /
                                               (1.0 - specularProbability);
                    } else {
                        diffuseIllumination += radiance * primarySurface.surface.albedo;
                    }
                } else {
                    if (statsOn) InterlockedAdd(traceStats[1], 1);
                    sampleSawSky = true;
                    if (sampleSpecular) {
                        specularIllumination += radiance * F / specularProbability;
                    } else if (envmapped) {
                        diffuseIllumination += radiance * primarySurface.surface.albedo * (1.0 - F) /
                                               (1.0 - specularProbability);
                    } else {
                        diffuseIllumination += radiance * primarySurface.surface.albedo;
                    }
                }
                if (sampleSawSky) skySamples += 1.0;
            }
        float emissionScale = (material.featureMask & kTraceSky) != 0
                                  ? pushConstants.skyIntensity
                                  : pushConstants.emissiveIntensity;
        color += primaryThroughput * (directIllumination +
                                      // Direct-light shadow rays are primary-hit only. Bounce
                                      // hits use the lightmap radiance cache below; tracing one
                                      // shadow ray per light at every bounce is not its budget.
                                      // The revision of the earlier omit-world-ambient
                                      // decision: windowless interiors measured at a third
                                      // of raster because this flat term dominates them.
                                      // (1 - skyFraction) keeps it out of open ground, so
                                      // the outdoor double-count that motivated the
                                      // omission still cannot happen.
                                      gammaToLinear(globalUniforms.worldAmbientColor.rgb) *
                                          (1.0 - skySamples / float(sampleCount)) *
                                          pushConstants.worldAmbientIntensity *
                                          primarySurface.surface.albedo +
                                      (diffuseIllumination + specularIllumination) / float(sampleCount) +
                                      primarySurface.surface.emissive * primarySurface.surface.albedo * emissionScale);
    }
    // Encode exactly once, at the very end, like the raster resolve.
    outputImage[dispatchId.xy] = float4(linearToGamma(color), 1.0);
}
