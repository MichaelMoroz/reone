/*
 * Copyright (c) 2026 The reone project contributors
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
#include "reone/scene/render/admission.h"

#include "reone/system/profiler.h"

#include <algorithm>
#include <limits>
#include <map>

#include "reone/graphics/mesh.h"
#include "reone/graphics/options.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/graphics/rhi/renderer.h"
#include "reone/graphics/rendering/pbrtextures.h"
#include "reone/scene/node/model.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone::scene {
namespace {
using InstanceMaterial = GpuScene::InstanceMaterial;
using AdmissionKind = GpuScene::AdmissionKind;

void hashBytes(uint64_t &hash, const void *data, size_t size) {
    constexpr uint64_t kPrime = 1099511628211ull;
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kPrime;
    }
}

template <typename T>
void hashVector(uint64_t &hash, const std::vector<T> &values) {
    if (!values.empty())
        hashBytes(hash, values.data(), values.size() * sizeof(T));
}

uint64_t hashUpload(const GpuSceneUpload &upload) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto &object : upload.objects)
        hashBytes(hash, &object.data, sizeof(object.data));
    hashVector(hash, upload.materials);
    hashVector(hash, upload.bones);
    hashVector(hash, upload.danglyPositions);
    hashVector(hash, upload.proceduralQuads);
    hashVector(hash, upload.grassFaces);
    hashVector(hash, upload.grassRanges);
    hashBytes(hash, &upload.cameraPosition, sizeof(upload.cameraPosition));
    hashBytes(hash, &upload.opaqueObjectCount, sizeof(upload.opaqueObjectCount));
    return hash;
}

std::string describeUploadDifference(const GpuSceneUpload &left,
                                     const GpuSceneUpload &right) {
    auto firstDifferent = [](const auto &a, const auto &b) -> size_t {
        const auto count = std::min(a.size(), b.size());
        for (size_t i = 0; i < count; ++i) {
            if (std::memcmp(&a[i], &b[i], sizeof(a[i])) != 0)
                return i;
        }
        return count;
    };
    const auto firstObject = firstDifferent(left.objects, right.objects);
    size_t objectByte = 0;
    uint32_t leftId = UINT32_MAX, rightId = UINT32_MAX;
    if (firstObject < left.objects.size() && firstObject < right.objects.size()) {
        leftId = left.objects[firstObject].objectIndex;
        rightId = right.objects[firstObject].objectIndex;
        const auto *a = reinterpret_cast<const unsigned char *>(
            &left.objects[firstObject].data);
        const auto *b = reinterpret_cast<const unsigned char *>(
            &right.objects[firstObject].data);
        while (objectByte < sizeof(SceneObject) && a[objectByte] == b[objectByte])
            ++objectByte;
    }
    std::string detail = " objects=" + std::to_string(left.objects.size()) + "/" +
                         std::to_string(right.objects.size()) +
                         " first_object=" + std::to_string(firstObject) +
                         " object_ids=" + std::to_string(leftId) + "/" +
                         std::to_string(rightId) +
                         " object_byte=" + std::to_string(objectByte) +
                         " materials=" + std::to_string(left.materials.size()) + "/" +
                         std::to_string(right.materials.size()) +
                         " first_material=" +
                         std::to_string(firstDifferent(left.materials, right.materials)) +
                         " bones=" + std::to_string(left.bones.size()) + "/" +
                         std::to_string(right.bones.size()) +
                         " first_bone=" +
                         std::to_string(firstDifferent(left.bones, right.bones)) +
                         " dangly=" + std::to_string(left.danglyPositions.size()) + "/" +
                         std::to_string(right.danglyPositions.size()) +
                         " first_dangly=" +
                         std::to_string(firstDifferent(left.danglyPositions,
                                                       right.danglyPositions)) +
                         " quads=" + std::to_string(left.proceduralQuads.size()) + "/" +
                         std::to_string(right.proceduralQuads.size()) +
                         " first_quad=" +
                         std::to_string(firstDifferent(left.proceduralQuads,
                                                       right.proceduralQuads)) +
                         " grass_faces=" + std::to_string(left.grassFaces.size()) + "/" +
                         std::to_string(right.grassFaces.size()) +
                         " first_grass_face=" +
                         std::to_string(firstDifferent(left.grassFaces,
                                                       right.grassFaces)) +
                         " grass_ranges=" + std::to_string(left.grassRanges.size()) + "/" +
                         std::to_string(right.grassRanges.size()) +
                         " first_grass_range=" +
                         std::to_string(firstDifferent(left.grassRanges,
                                                       right.grassRanges));
    return detail;
}

void applyCategoryOverride(InstanceMaterial &material,
                           const GraphicsOptions &options,
                           uint32_t categoryIndex) {
    const auto &src = options.categoryOverrides[std::min<uint32_t>(categoryIndex, 8u)];
    material.overrideColor = glm::vec4(src.color[0], src.color[1], src.color[2],
                                       std::clamp(src.colorWeight, 0.0f, 1.0f));
    material.overrideParams = glm::vec4(src.roughness,
                                        std::max(0.0f, src.emissionScale),
                                        std::max(0.0f, src.envScale),
                                        std::max(0.0f, src.metallicScale));
    material.roughnessScale = std::max(0.0f, src.roughnessScale);
}

void populateMaterialResources(InstanceMaterial &dst,
                               const Material &src,
                               IRenderer &renderer,
                               const GraphicsOptions &options) {
    const auto textureAt = [&src](MaterialTextureSlot slot) {
        return src.textures[static_cast<size_t>(slot)];
    };
    const auto textureId = [&renderer](const Texture *texture) {
        return texture ? renderer.resources().textureId(*texture).value_or(UINT32_MAX)
                       : UINT32_MAX;
    };

    const auto *mainTex = textureAt(MaterialTextureSlot::MainTex);
    dst.mainTex = textureId(mainTex);
    dst.normalMap = textureId(textureAt(MaterialTextureSlot::NormalMap));
    // The diagnostic lightmaps toggle strips the map at the material record,
    // so every mode - raster and traced - sees the same lightmap-free scene
    // and the cross-mode upload-hash equality is preserved within a run.
    dst.lightmap = options.lightmaps ? textureId(textureAt(MaterialTextureSlot::Lightmap)) : UINT32_MAX;

    if (const auto *bumpMap = textureAt(MaterialTextureSlot::BumpMapArray)) {
        dst.bumpMapArray = textureId(bumpMap);
        dst.bumpMapFrame = src.bumpMapFrame;
        dst.bumpMapScale = bumpMap->features().bumpMapScaling;
    }
    if (mainTex && mainTex->features().waterAlpha != -1.0f) {
        dst.waterAlpha = mainTex->features().waterAlpha;
    }

    auto *envMap = textureAt(MaterialTextureSlot::EnvMap);
    auto *envMapCube = textureAt(MaterialTextureSlot::EnvMapCube);
    dst.envMap = textureId(envMap);
    dst.envMapCube = textureId(envMapCube);
    if (auto *derivedSource = envMapCube ? envMapCube : envMap) {
        dst.envMapDerivedLayer =
            renderer.pbrTextures().requestEnvMapDerivedLayer(*derivedSource);
    }
}

} // namespace

GpuSceneAdmission::GpuSceneAdmission(IRenderer &renderer,
                                     GraphicsOptions &options,
                                     GpuScene &gpuScene) :
    _renderer(renderer), _options(options), _gpuScene(gpuScene) {}

std::optional<GpuScene::Classification> GpuSceneAdmission::classifyMesh(
    const RegisteredMesh &mesh, const ModelSceneNode *skyRoom) {
    // A selected sky shell is an environment source, never merged geometry.
    // This is unconditional: a failed bake uses the fallback cube.
    if (skyRoom && mesh.cullRoot == skyRoom) {
        ++_submission.sky;
        return std::nullopt;
    }
    const bool saber = std::holds_alternative<RegisteredSaber>(mesh.deformation);
    const bool dangly = std::holds_alternative<RegisteredDangly>(mesh.deformation);
    const auto *skinned = std::get_if<RegisteredSkin>(&mesh.deformation);
    if (!std::holds_alternative<std::monostate>(mesh.deformation) &&
        !skinned && !saber && !dangly) {
        ++_submission.deforming;
        return std::nullopt;
    }
    if (saber)
        ++_submission.sabers;
    if (dangly)
        ++_submission.dangly;
    if (mesh.id.index > 0x00ffffffu) {
        ++_submission.outOfRange;
        return std::nullopt;
    }
    if (skinned)
        ++_submission.skinned;

    InstanceMaterial material;
    material.selfIllumColor = glm::vec4(mesh.material.selfIllumColor, 0.0f);
    // The merged vertex stream has no per-object colour. Preserve the model's
    // alpha controller in the material record for forward compositing.
    material.diffuseColor =
        glm::vec4(mesh.material.diffuseColor, mesh.material.color.a);
    material.ambientColor = glm::vec4(mesh.material.ambientColor, 1.0f);
    material.uv0 = mesh.material.uv[0];
    material.uv1 = mesh.material.uv[1];
    material.uv2 = mesh.material.uv[2];
    material.featureMask = static_cast<uint32_t>(materialFeatureMask(mesh.material));
    populateMaterialResources(material, mesh.material, _renderer, _options);
    const uint32_t categoryIndex = mesh.cullRoot
                                       ? static_cast<uint32_t>(mesh.cullRoot->usage())
                                       : 8u;
    material.featureMask |= (categoryIndex & 0xFu) << 27;

    // Bit 24 remains reserved and wired through the shaders for V5. Selected
    // sky meshes return above, so no admitted object currently receives it.
    if (skyRoom && mesh.cullRoot == skyRoom)
        material.featureMask |= 1u << 24;

    const auto *curated = _gpuScene.traceMaterials().curatedByIndex(mesh.material.curatedIndex);
    if (dangly && (!curated || curated->klass != TraceClass::Emissive))
        material.selfIllumColor = glm::vec4(0.0f);
    if (curated) {
        if (curated->klass == TraceClass::None)
            material.selfIllumColor = glm::vec4(0.0f);
        material.curatedAlbedoMul = glm::vec4(curated->albedoMul, 0.0f);
        material.curatedRoughA = glm::vec4(static_cast<float>(curated->roughnessMode),
                                           curated->roughnessParams.x,
                                           curated->roughnessParams.y,
                                           curated->roughnessParams.z);
        material.curatedRoughB = glm::vec4(curated->roughnessParams.w,
                                           curated->roughnessWeights.x,
                                           curated->roughnessWeights.y,
                                           curated->roughnessWeights.z);
        material.curatedMetalA = glm::vec4(static_cast<float>(curated->metallicMode),
                                           curated->metallicParams.x,
                                           curated->metallicParams.y,
                                           curated->metallicParams.z);
        material.curatedMetalB = glm::vec4(curated->metallicParams.w,
                                           curated->metallicWeights.x,
                                           curated->metallicWeights.y,
                                           curated->metallicWeights.z);
        material.curatedEmission = glm::vec4(curated->emissionValue,
                                             static_cast<float>(curated->emissionMode));
    }

    AdmissionKind kind = AdmissionKind::Opaque;
    if (const auto *diffuse =
            mesh.material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)]) {
        if (diffuse->features().blending == Texture::Blending::Additive) {
            kind = AdmissionKind::AdditiveEmissive;
            ++_submission.additive;
        } else if (diffuse->features().blending == Texture::Blending::PunchThrough ||
                   mesh.material.type == MaterialType::TransparentModel) {
            // TransparentModel stays a cutout. It is not a statement that the
            // surface has continuous coverage: MeshSceneNode::isTransparent
            // falls through to hasAlphaChannel, so any texture that merely
            // carries an alpha channel lands here - which is most foliage.
            // Routing those into the blended pass takes trees out of the
            // G-buffer and visibly changes geometry that was correct before.
            // Genuinely blended meshes need a narrower signal than this flag
            // before they can be separated from alpha-tested ones.
            kind = AdmissionKind::Cutout;
        }
    }
    // Backdrop cutouts are sky, and shaded as sky.
    //
    // The painted skyline: background scenery drawn as an alpha cutout rather
    // than modelled. Measured on Taris upper city, three meshes in the whole
    // module - m02ab_02l/line1650, line1651, line2045 - each carrying
    // selfIllum 1,1,1, which made them ordinary lit geometry that also glowed,
    // graded by the emissive dial. At an emissive intensity of 3.59 they came
    // out white.
    //
    // They take the sky's dial rather than one of their own because that is
    // what they are: far enough that their parallax does not matter, and drawn
    // over the sky, so any brightness that is not the sky's reads as a seam.
    //
    // This is a shading classification, not a bake. The sky bake gathers by
    // membership in the sky room, which these are not in - they stay admitted,
    // rasterized and traced like the geometry they are. The bit only selects
    // the unlit surface model and the sky intensity.
    //
    // Cutout is the discriminator, and it is the whole of it: the other 132
    // self-illuminated scenery meshes in that module are modelled buildings,
    // which are lit normally and stay that way.
    if (mesh.material.backgroundGeometry && kind == AdmissionKind::Cutout) {
        material.featureMask |= 1u << 24;
    }
    if ((material.featureMask & (1u << 24)) != 0 ||
        (curated && curated->klass == TraceClass::Prelit)) {
        material.surfaceType = 2;
    }

    applyCategoryOverride(material, _options, categoryIndex);
    const float emissionScale = std::max(0.0f, material.overrideParams.y);
    // Additive emission reads overrideParams.y directly. Curated prelit still
    // takes the old baked scaling path because it terminates instead.
    if (kind != AdmissionKind::AdditiveEmissive || material.surfaceType == 2)
        material.selfIllumColor *= emissionScale;
    if (curated && curated->emissionMode != 0) {
        material.curatedEmission =
            glm::vec4(glm::vec3(material.curatedEmission) * emissionScale,
                      material.curatedEmission.w);
    }

    _submission.dynamicTriangles +=
        (skinned || dangly || saber) ? static_cast<uint32_t>(mesh.mesh.get().faces().size()) : 0;
    if (!dangly && (!curated || curated->klass == TraceClass::Default) &&
        glm::any(glm::greaterThan(mesh.material.selfIllumColor, glm::vec3(0.0f)))) {
        ++_submission.emissive;
    }
    return {{material, kind, GpuScene::ResidencyClass::Dynamic, skinned}};
}

namespace {

/** The shape dials, as the merge kernel takes them. */
graphics::GrassParams grassParamsFrom(const graphics::GraphicsOptions &options) {
    graphics::GrassParams params;
    params.radius = std::max(0.0f, options.grassRadius);
    params.windStrength = options.grassWindStrength;
    params.windDirection = options.grassWindDirection;
    params.windSpeed = options.grassWindSpeed;
    params.windWavelength = std::max(0.01f, options.grassWindWavelength);
    params.windGust = std::clamp(options.grassWindGust, 0.0f, 1.0f);
    params.orientation = options.grassOrientation;
    params.orientationVariance = std::max(0.0f, options.grassOrientationVariance);
    params.curvature = options.grassCurvature;
    params.curvatureVariance = std::max(0.0f, options.grassCurvatureVariance);
    params.sparsity = std::clamp(options.grassSparsity, 0.0f, 0.99f);
    params.displacement = std::max(0.0f, options.grassDisplacement);
    params.length = std::max(0.0f, options.grassLength);
    params.lengthVariance = std::clamp(options.grassLengthVariance, 0.0f, 1.0f);
    params.width = std::max(0.0f, options.grassWidth);
    params.yOffset = options.grassYOffset;
    params.roughness = std::clamp(options.grassRoughness, 0.0f, 1.0f);
    params.bladesPerCluster = static_cast<uint32_t>(std::clamp(options.grassBladesPerCluster, 1, 32));
    // The ceiling as blades, which is the unit the allocator works in. Zero
    // means no ceiling, and the density dials answer for the triangle count.
    params.segments = static_cast<uint32_t>(
        std::clamp<int>(options.grassSegments, graphics::kMinGrassSegments,
                        graphics::kMaxGrassSegments));
    params.budgetBlades =
        options.grassTriangleBudget > 0
            ? static_cast<uint32_t>(options.grassTriangleBudget /
                                    graphics::grassTrisPerBlade(params.segments))
            : 0u;
    // Not clamped to 1. One is the density the faces were authored at, and
    // stopping there made the authored budget a second ceiling: raising the
    // triangle budget past what the bake happens to contain then did nothing,
    // which is exactly what it looked like. Cluster indices are hashed, so
    // there is nothing special about the authored count - it is a weight, not a
    // limit.
    params.density = std::max(0.0f, options.grassDensity / kGrassDensityCap);
    params.color = glm::vec4(glm::max(options.grassColor, glm::vec3(0.0f)), 1.0f);
    // Retro draws what the original drew. It is a raster mode, so the cutout
    // never reaches the tracer and costs its shadow rays nothing.
    params.cardboard = options.mode == graphics::RenderMode::Retro ? 1u : 0u;
    return params;
}

/**
 * The fraction of each face's baked budget that survives, as the merge kernel
 * reads it from cameraPosition.w.
 *
 * Two ceilings share one number. The density dial is the author's, expressed
 * against the cap the budgets were baked at. The triangle budget is the
 * machine's, and what it costs is not the raster - it is the acceleration
 * structure this geometry has to be rebuilt into. Taking the lower of the two
 * lets both ride the prefix gate that already exists, so neither costs a
 * rebuild of the face records.
 */
float grassDensityFraction(const graphics::GraphicsOptions &options, size_t areaBlades) {
    const float byDensity = std::clamp(options.grassDensity / kGrassDensityCap, 0.0f, 1.0f);
    if (areaBlades == 0 || options.grassTriangleBudget <= 0) {
        return byDensity;
    }
    // Against the area's whole blade count, not the currently admitted one.
    // Dividing by what happens to be admitted made the ceiling a function of
    // where the camera was standing: the surviving prefix moved as you walked
    // and blades appeared and vanished across the entire field. The area's
    // total is fixed for as long as the module is loaded, so the same blade
    // survives or does not regardless of where it is seen from.
    const bool retro = options.mode == graphics::RenderMode::Retro;
    const float perCluster =
        retro ? 2.0f
              : static_cast<float>(graphics::grassTrisPerBlade(static_cast<uint32_t>(
                    std::clamp<int>(options.grassSegments, graphics::kMinGrassSegments,
                                    graphics::kMaxGrassSegments)))) *
                    static_cast<float>(std::clamp(options.grassBladesPerCluster, 1, 32));
    const float allowed = static_cast<float>(options.grassTriangleBudget) / perCluster;
    return std::min(byDensity, std::clamp(allowed / static_cast<float>(areaBlades), 0.0f, 1.0f));
}

} // namespace


std::optional<GpuScene::Classification> GpuSceneAdmission::classifyProcedural(
    const RegisteredProcedural &procedural) {
    InstanceMaterial material;
    AdmissionKind kind = AdmissionKind::Opaque;
    switch (procedural.kind) {
    case ProceduralKind::Grass:
        material.diffuseColor = glm::vec4(procedural.material.diffuseColor, 1.0f);
        material.ambientColor = glm::vec4(procedural.material.ambientColor, 1.0f);
        material.uv0 = procedural.material.uv[0];
        material.uv1 = procedural.material.uv[1];
        material.uv2 = procedural.material.uv[2];
        {
            // Strands carry no texture and no alpha, so they are ordinary
            // opaque geometry. That is most of the point: a cutout cannot be
            // committed by the hardware, so every shadow ray crossing a grass
            // field ran the any-hit loop, fetched the material and alpha-tested
            // it, once per blade it touched.
            //
            // Retro still draws the cardboard, and cardboard is still a cutout.
            const bool strands = _options.mode != graphics::RenderMode::Retro;
            uint32_t features = static_cast<uint32_t>(materialFeatureMask(procedural.material));
            if (!strands) {
                features |= UniformsFeatureFlags::hashedalphatest;
            }
            // Grass is the archetype of a thin surface, and it receives
            // shadows. Neither is authored: the area records grass as a
            // decoration, so nothing in the source data asks for either, and
            // without them a field is lit as though every blade were a solid
            // slab that nothing can fall across.
            // Fog too, for the same reason: grass is world geometry standing on
            // terrain that fogs, and an area records it as decoration, so
            // nothing in the source data asks for it. Unfogged blades in front
            // of fogged ground read as a hole in the weather.
            features |= UniformsFeatureFlags::thin | UniformsFeatureFlags::shadows |
                        UniformsFeatureFlags::fog;
            material.featureMask = features | (8u << 27);
            applyCategoryOverride(material, _options, 8);
            kind = strands ? AdmissionKind::Opaque : AdmissionKind::Cutout;
        }
        break;
    case ProceduralKind::Particles:
        _submission.particles += static_cast<uint32_t>(procedural.instanceCount());
        material.diffuseColor = glm::vec4(procedural.material.diffuseColor, 1.0f);
        material.ambientColor = glm::vec4(procedural.material.ambientColor, 1.0f);
        material.uv0 = procedural.material.uv[0];
        material.uv1 = procedural.material.uv[1];
        material.uv2 = procedural.material.uv[2];
        material.featureMask = static_cast<uint32_t>(materialFeatureMask(procedural.material));
        applyCategoryOverride(material, _options, 8);
        kind = procedural.material.blending == BlendMode::Lighten
                   ? AdmissionKind::AdditiveEmissive
                   : AdmissionKind::LitBlended;
        break;
    case ProceduralKind::Billboard:
        ++_submission.billboards;
        // The lowered quad already carries the flare colour and alpha. Keep
        // material alpha neutral so forward blending applies it only once.
        material.diffuseColor =
            glm::vec4(glm::vec3(procedural.instances.front().color), 1.0f);
        material.ambientColor = glm::vec4(procedural.material.ambientColor, 1.0f);
        kind = AdmissionKind::AdditiveEmissive;
        break;
    }
    populateMaterialResources(material, procedural.material, _renderer, _options);
    if (procedural.kind == ProceduralKind::Grass &&
        _options.mode != graphics::RenderMode::Retro) {
        // A strand has no texture. The area's grass image is a picture of
        // cardboard blades, and mapping it across a blade puts a vertical slice
        // of that picture on every one - which modulates the configured colour
        // by whatever happened to be in that column, and reads as the colour
        // dial not working. Retro keeps it, because cardboard is what the
        // texture is for.
        material.mainTex = UINT32_MAX;
        material.diffuseColor = glm::vec4(1.0f);
        material.overrideParams.x = std::clamp(_options.grassRoughness, 0.0f, 1.0f);
    }
    return {{material, kind, GpuScene::ResidencyClass::Dynamic, nullptr}};
}

void GpuSceneAdmission::rebuildSubmissionCounts(const ModelSceneNode *skyRoom) {
    auto upload = std::move(_submission.upload);
    _submission = {};
    _submission.upload = std::move(upload);
    const auto visibleCategories = renderCategory(RenderCategory::Opaque) |
                                   renderCategory(RenderCategory::Transparent);
    for (const auto &object : _gpuScene.objects()) {
        const auto id = std::visit([](const auto &entry) { return entry.id; }, object);
        if (!_gpuScene.isObjectActive(id) || !_gpuScene.isObjectEnabled(id.index))
            continue;
        if (const auto *mesh = std::get_if<RegisteredMesh>(&object)) {
            if ((mesh->categories & visibleCategories) == 0)
                continue;
            if (skyRoom && mesh->cullRoot == skyRoom) {
                ++_submission.sky;
                continue;
            }
            if (mesh->id.index > 0x00ffffffu) {
                ++_submission.outOfRange;
                continue;
            }
            const bool skinned = std::holds_alternative<RegisteredSkin>(mesh->deformation);
            const bool dangly = std::holds_alternative<RegisteredDangly>(mesh->deformation);
            const bool saber = std::holds_alternative<RegisteredSaber>(mesh->deformation);
            _submission.skinned += skinned ? 1u : 0u;
            _submission.dangly += dangly ? 1u : 0u;
            _submission.sabers += saber ? 1u : 0u;
            if (skinned || dangly || saber)
                _submission.dynamicTriangles +=
                    static_cast<uint32_t>(mesh->mesh.get().faces().size());
            if (const auto *diffuse = mesh->material.textures[
                    static_cast<size_t>(MaterialTextureSlot::MainTex)];
                diffuse && diffuse->features().blending == Texture::Blending::Additive)
                ++_submission.additive;
            const auto *curated =
                _gpuScene.traceMaterials().curatedByIndex(mesh->material.curatedIndex);
            if (!dangly && (!curated || curated->klass == TraceClass::Default) &&
                glm::any(glm::greaterThan(mesh->material.selfIllumColor,
                                          glm::vec3(0.0f))))
                ++_submission.emissive;
            continue;
        }
        const auto &procedural = std::get<RegisteredProcedural>(object);
        if ((procedural.categories & visibleCategories) == 0 ||
            procedural.instanceCount() == 0)
            continue;
        switch (procedural.kind) {
        case ProceduralKind::Grass:
            break;
        case ProceduralKind::Particles:
            _submission.particles += static_cast<uint32_t>(procedural.instanceCount());
            break;
        case ProceduralKind::Billboard:
            ++_submission.billboards;
            break;
        }
    }
    for (const auto &range : _submission.upload.grassRanges)
        _submission.grass += range.clusterCount;
}

GpuSceneAdmissionResult GpuSceneAdmission::prepare(
    const glm::mat4 &view,
    graphics::GpuSceneUpload reuse) {
    R_PROFILE_ZONE("SceneAdmission::prepare");
    _submission = {};

    uint64_t optionsFingerprint = 14695981039346656037ull;
    hashBytes(optionsFingerprint, _options.categoryOverrides,
              sizeof(_options.categoryOverrides));
    // Every option that reaches a material record has to be in here, or the
    // classification cache answers from before the change and the dial sits
    // inert. The lightmaps toggle strips the map at the record (see
    // applyCategoryOverride's neighbourhood below), so it belongs.
    hashBytes(optionsFingerprint, &_options.lightmaps, sizeof(_options.lightmaps));
    if (_optionsFingerprint != 0 && _optionsFingerprint != optionsFingerprint)
        ++_admissionGeneration;
    _optionsFingerprint = optionsFingerprint;

    GpuSceneAdmissionResult result;
    const auto sceneGeneration = _gpuScene.admissionGeneration();
    if (_skyCacheGeneration != sceneGeneration) {
        static constexpr float kSkyOverlapThreshold = 0.5f;
        glm::vec3 sceneMin(std::numeric_limits<float>::max());
        glm::vec3 sceneMax(std::numeric_limits<float>::lowest());
        struct SkyRoomCandidate {
            glm::vec3 boundsMin {std::numeric_limits<float>::max()};
            glm::vec3 boundsMax {std::numeric_limits<float>::lowest()};
        };
        std::map<const ModelSceneNode *, SkyRoomCandidate> sceneryRooms;
        for (const auto &object : _gpuScene.objects()) {
            const auto *mesh = std::get_if<RegisteredMesh>(&object);
            if (!mesh || !mesh->cullRoot || mesh->cullRoot->usage() != ModelUsage::Room)
                continue;
            if (!_gpuScene.isObjectEnabled(mesh->id.index))
                continue;
            if ((mesh->categories & (renderCategory(RenderCategory::Opaque) |
                                     renderCategory(RenderCategory::Transparent))) == 0)
                continue;
            const auto worldAabb = mesh->mesh.get().aabb() * mesh->transform;
            sceneMin = glm::min(sceneMin, worldAabb.min());
            sceneMax = glm::max(sceneMax, worldAabb.max());
            if (mesh->cullRoot->isBackgroundScenery()) {
                auto &candidate = sceneryRooms[mesh->cullRoot];
                candidate.boundsMin = glm::min(candidate.boundsMin, worldAabb.min());
                candidate.boundsMax = glm::max(candidate.boundsMax, worldAabb.max());
            }
        }
        if (!sceneryRooms.empty()) {
            const glm::vec3 sceneExtent =
                glm::max(sceneMax - sceneMin, glm::vec3(1e-3f));
            float bestVolume = 0.0f;
            for (const auto &[room, candidate] : sceneryRooms) {
                const glm::vec3 extent = candidate.boundsMax - candidate.boundsMin;
                const glm::vec3 ratios = extent / sceneExtent;
                if (glm::min(ratios.x, glm::min(ratios.y, ratios.z)) <
                    kSkyOverlapThreshold)
                    continue;
                const float volume = extent.x * extent.y * extent.z;
                if (volume > bestVolume) {
                    bestVolume = volume;
                    result.skyRoom = room;
                    result.skyOrigin = 0.5f * (candidate.boundsMin + candidate.boundsMax);
                }
            }
        }
        _cachedSkyRoom = result.skyRoom;
        _cachedSkyOrigin = result.skyOrigin;
        _skyCacheGeneration = sceneGeneration;
        if (result.skyRoom && _frameNumber == 0) {
            info("Scene admission: sky room is '" + result.skyRoom->model().name() + "'",
                 LogChannel::Graphics);
        }
    } else {
        result.skyRoom = _cachedSkyRoom;
        result.skyOrigin = _cachedSkyOrigin;
    }

    if (_classifiedSkyRoom != result.skyRoom) {
        ++_admissionGeneration;
        _classifiedSkyRoom = result.skyRoom;
    }
    _gpuScene.setSkyRoom(result.skyRoom);
    _gpuScene.setGrassParams(grassParamsFrom(_options));
    _submission.upload = _gpuScene.prepare(
        [this, skyRoom = result.skyRoom](const RegisteredMesh &mesh) {
            return classifyMesh(mesh, skyRoom);
        },
        [this](const RegisteredProcedural &procedural) {
            return classifyProcedural(procedural);
        },
        view, _admissionGeneration,
        _options.admissionForceFull || _gpuScene.consumeGrassPrimitiveChange(),
        std::move(reuse));
    // The live grass density rides to the merge kernel in cameraPosition.w as
    // density/cap; budgets are baked at the cap. Set before hashing so the
    // shadow path (below, same assignment) stays byte-comparable.
    for (const auto &range : _submission.upload.grassRanges)
        _submission.grass += range.clusterCount;
    _submission.upload.cameraPosition.w =
        grassDensityFraction(_options, _gpuScene.counts().grassClusters);
    if (_options.admissionShadow && _gpuScene.shadowScene()) {
        auto savedSubmission = _submission;
        _gpuScene.shadowScene()->setGrassParams(grassParamsFrom(_options));
        auto shadowUpload = _gpuScene.shadowScene()->prepare(
            [this, skyRoom = result.skyRoom](const RegisteredMesh &mesh) {
                return classifyMesh(mesh, skyRoom);
            },
            [this](const RegisteredProcedural &procedural) {
                return classifyProcedural(procedural);
            },
            view, _admissionGeneration, true);
        _submission = std::move(savedSubmission);
        // The same number, so the shadow scene admits the same blades and the
        // upload comparison stays byte-for-byte.
        shadowUpload.cameraPosition.w =
            grassDensityFraction(_options, _gpuScene.counts().grassClusters);
        const auto incrementalHash = hashUpload(_submission.upload);
        const auto shadowHash = hashUpload(shadowUpload);
        if (_submission.upload.objects.empty() != shadowUpload.objects.empty()) {
            // Module loading can update deformers one engine tick before the
            // graph publishes its first complete render lists. Do not let that
            // partial tick seed different persistent intern histories.
            _gpuScene.resetAdmissionCache();
            _gpuScene.shadowScene()->resetAdmissionCache();
            info("GpuScene admission shadow warmup frame=" +
                     std::to_string(_frameNumber),
                 LogChannel::Graphics);
        } else if (incrementalHash != shadowHash) {
            error("GpuScene admission shadow MISMATCH frame=" +
                      std::to_string(_frameNumber) + ", incremental=" +
                      std::to_string(incrementalHash) + ", full=" +
                      std::to_string(shadowHash) +
                      describeUploadDifference(_submission.upload, shadowUpload),
                  LogChannel::Graphics);
        } else {
            info("GpuScene admission shadow match frame=" +
                     std::to_string(_frameNumber) + ", hash=" +
                     std::to_string(incrementalHash),
                 LogChannel::Graphics);
        }
    }
    if (_options.admissionShadow || _options.hashUploads ||
        Logger::instance.isChannelEnabled(LogChannel::Graphics))
        rebuildSubmissionCounts(result.skyRoom);
    // The hash walks every uploaded byte; its one consumer is the
    // --dumptargets log line, so frames outside a dump run skip it.
    result.uploadHash = _options.hashUploads ? hashUpload(_submission.upload) : 0;
    result.submission = std::move(_submission);
    ++_frameNumber;
    return result;
}

} // namespace reone::scene
