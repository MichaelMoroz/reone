// Primary-ray path tracer for Vulkan ray query.  Every hit reads its material
// and vertex attributes from GPU-resident data; no raster draw state is used.
import uniforms;
import lib.surface;

[[vk::binding(0, 1)]] RWTexture2D<float4> outputImage;
[[vk::binding(1, 1)]] RaytracingAccelerationStructure sceneTLAS;

static const uint kNoTexture = 0xffffffffu;

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
    // World-space offset of a secondary ray's origin along the geometric
    // normal. Too small self-intersects and speckles black; too large leaks
    // light through contact edges. A dial because the right value is
    // scene-scale-dependent and staring at acne beats recompiling.
    float rayOriginOffset;
};
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
    layer.albedo = material.diffuseColor.rgb;
    layer.alpha = 1.0;
    float2 uv = float2(0.0);
    if (material.offUV1 != -1) {
        uv = transformedUV(material, interpolateFloat2(material, indices, material.offUV1, weights));
    }
    if (material.mainTex != kNoTexture) {
        float4 sample = bindlessTextures[NonUniformResourceIndex(material.mainTex)].SampleLevel(uv, 0.0);
        layer.albedo = sample.rgb;
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
        surface.albedo = mainTexSample.rgb;
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
            surface.lightmap = bindlessTextures[NonUniformResourceIndex(material.lightmap)].SampleLevel(uv2, 0.0).rgb;
        }
    }
    surface.emissive = material.selfIllumColor.rgb;
    if ((material.featureMask & kTraceAdditive) != 0) {
        // Emission of 1 makes the additive texture read at its own brightness
        // through the emission*albedo path, matching how raster composites it.
        surface.emissive = max(surface.emissive, float3(1.0));
    }
    hit.surface = surface;
    return hit;
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
    for (uint segment = 0; segment < kMaxTransparentLayers; ++segment) {
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
                // Addition is commutative, so additive surfaces composite in
                // candidate order. They see the segment-entry throughput: a
                // blade behind a leaf in the same segment reads slightly
                // bright. Bounded, and sabers rarely hide in trees.
                primaryLayers += primaryThroughput * additiveRadiance(candidateMaterial, layer);
            } else if ((candidateMaterial.featureMask & kTracePunchThrough) != 0) {
                // Commit anything with meaningful coverage; the query keeps
                // the nearest. Fully transparent texels never commit and cost
                // nothing further.
                if (layer.alpha > 0.01) {
                    primary.CommitNonOpaqueTriangleHit();
                }
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
        float3 specularIllumination = 0.0;
        float3 V = -ray.Direction;
        bool envmapped = (material.featureMask & uint(kFeatureEnvMap)) != 0;
        float envMapStrength = envmapped ? (1.0 - primarySurface.surface.alpha) : 0.0;
        float3 F0 = lerp(lerp(float3(0.04), primarySurface.surface.albedo,
                               primarySurface.surface.metallic),
                          float3(1.0), envMapStrength);
        float3 F = fresnelSchlick(max(0.0, dot(primarySurface.surface.normal, V)), F0);
        // The GGX lobe exists only where raster shows this heuristic a
        // specular response: environment-mapped surfaces. Everywhere else the
        // diffuse alpha is coverage or opacity, so the derived roughness is
        // meaningless - grass texels with soft alpha turned glossy and the
        // whole frame sparkled. Non-envmapped surfaces are pure diffuse.
        float specularProbability = envmapped ? clamp(luminance(F), 1e-3, 1.0 - 1e-3) : 0.0;
            uint sampleCount = max(pushConstants.samplesPerPixel, 1u);
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
                        bounceLayers += bounceThroughput * additiveRadiance(candidateMaterial, layer);
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
                InterlockedAdd(traceStats[0], 1);
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
                    InterlockedAdd(traceStats[1], 1);
                    if (sampleSpecular) {
                        specularIllumination += radiance * F / specularProbability;
                    } else if (envmapped) {
                        diffuseIllumination += radiance * primarySurface.surface.albedo * (1.0 - F) /
                                               (1.0 - specularProbability);
                    } else {
                        diffuseIllumination += radiance * primarySurface.surface.albedo;
                    }
                }
            }
        float emissionScale = (material.featureMask & kTraceSky) != 0
                                  ? pushConstants.skyIntensity
                                  : pushConstants.emissiveIntensity;
        color += primaryThroughput * ((diffuseIllumination + specularIllumination) / float(sampleCount) +
                                      primarySurface.surface.emissive * primarySurface.surface.albedo * emissionScale);
    }
    outputImage[dispatchId.xy] = float4(color, 1.0);
}
