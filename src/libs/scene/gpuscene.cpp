/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/scene/gpuscene.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>

#include "reone/graphics/mesh.h"
#include "reone/scene/node/model.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone::scene {
namespace {
GpuScene::Matrix3x4 matrix3x4(const glm::mat4 &m) {
    return {{m[0][0], m[1][0], m[2][0], m[3][0]},
            {m[0][1], m[1][1], m[2][1], m[3][1]},
            {m[0][2], m[1][2], m[2][2], m[3][2]}};
}

bool isCountedMesh(const Material &material) {
    return material.type == MaterialType::OpaqueModel ||
           material.type == MaterialType::TransparentModel;
}

void countMesh(SceneCounts &counts, const RegisteredDeformation &deformation) {
    if (std::holds_alternative<RegisteredSkin>(deformation))
        ++counts.skinned;
    else if (std::holds_alternative<RegisteredDangly>(deformation))
        ++counts.dangly;
    else if (std::holds_alternative<RegisteredSaber>(deformation))
        ++counts.saber;
    else
        ++counts.rigid;
}

uint64_t hashBytes(const void *data, size_t size) {
    constexpr uint64_t kOffset = 14695981039346656037ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t hash = kOffset;
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kPrime;
    }
    return hash;
}

class MaterialPool {
public:
    explicit MaterialPool(graphics::GpuSceneUpload &upload) :
        _upload(upload) {}

    uint32_t add(const GpuScene::InstanceMaterial &material) {
        ++_upload.materialReferenceCount;
        const auto hash = hashBytes(&material, sizeof(material));
        auto &candidates = _indices[hash];
        for (const auto index : candidates) {
            if (std::memcmp(&_upload.materials[index], &material, sizeof(material)) == 0)
                return index;
        }
        const auto index = static_cast<uint32_t>(_upload.materials.size());
        _upload.materials.push_back(material);
        candidates.push_back(index);
        return index;
    }

private:
    graphics::GpuSceneUpload &_upload;
    std::unordered_map<uint64_t, std::vector<uint32_t>> _indices;
};

GpuScene::PrimitiveClass applyAdmissionKind(GpuScene::Classification &classification) {
    constexpr uint32_t kBlendedCoverage = 1u << 25;
    constexpr uint32_t kPunchThrough = 1u << 26;
    classification.material.featureMask &= ~(kBlendedCoverage | kPunchThrough);
    switch (classification.kind) {
    case GpuScene::AdmissionKind::Opaque:
        return GpuScene::PrimitiveClass::Opaque;
    case GpuScene::AdmissionKind::Cutout:
        classification.material.featureMask |= kPunchThrough;
        return GpuScene::PrimitiveClass::NonOpaque;
    case GpuScene::AdmissionKind::LitBlended:
        classification.material.featureMask |= kBlendedCoverage;
        return GpuScene::PrimitiveClass::NonOpaque;
    case GpuScene::AdmissionKind::AdditiveEmissive:
        // Curated prelit remains the existing terminating surface model.
        if (classification.material.surfaceType != 2)
            classification.material.surfaceType = 1;
        return GpuScene::PrimitiveClass::NonOpaque;
    }
    return GpuScene::PrimitiveClass::Opaque;
}

} // namespace

std::string formatSceneCounts(const SceneCounts &counts) {
    return "objects=" + std::to_string(counts.objects()) +
           ", rigid=" + std::to_string(counts.rigid) +
           ", skinned=" + std::to_string(counts.skinned) +
           ", dangly=" + std::to_string(counts.dangly) +
           ", saber=" + std::to_string(counts.saber) +
           ", particle_emitters=" + std::to_string(counts.particleEmitters) +
           ", particles=" + std::to_string(counts.particles) +
           ", grass_nodes=" + std::to_string(counts.grassNodes) +
           ", grass_clusters=" + std::to_string(counts.grassClusters) +
           ", billboards=" + std::to_string(counts.billboards);
}

void GpuScene::resetFrame() {
    _objects.clear();
    _counts = {};
}

void GpuScene::checkIdentityStability() {
    if (!Logger::instance.isChannelEnabled(LogChannel::Graphics))
        return;
    std::vector<SceneNodeId> ids;
    ids.reserve(_objects.size());
    for (const auto &object : _objects)
        std::visit([&ids](const auto &entry) { ids.push_back(entry.id); }, object);
    std::sort(ids.begin(), ids.end(), [](SceneNodeId a, SceneNodeId b) {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    });
    const bool unique = std::adjacent_find(ids.begin(), ids.end()) == ids.end();
    const bool sameAsPrevious = !_previousFrameIds.empty() && ids == _previousFrameIds;
    info("GpuScene classification ids snapshot=" + std::to_string(++_identitySnapshot) +
             ", entries=" + std::to_string(ids.size()) +
             ", unique=" + (unique ? "true" : "false") +
             ", same_as_previous=" + (sameAsPrevious ? "true" : "false"),
         LogChannel::Graphics);
    _previousFrameIds = std::move(ids);
}

void GpuScene::addMesh(RenderCategories categories, SceneNodeId id,
                       SceneNodeNameIds nameIds, Mesh &mesh, const Material &material,
                       const glm::mat4 &transform, const glm::mat4 &transformInv,
                       const glm::mat4 &prevTransform, RegisteredDeformation deformation,
                       ModelSceneNode *cullRoot) {
    ++_counts.entries;
    if (isCountedMesh(material))
        countMesh(_counts, deformation);
    _objects.push_back(RegisteredMesh {categories, id, nameIds, mesh, material, transform,
                                       transformInv, prevTransform, std::move(deformation), cullRoot});
}

void GpuScene::addBillboard(RenderCategories categories, SceneNodeId id,
                            SceneNodeNameIds nameIds, Texture &texture, const glm::vec4 &color,
                            const glm::mat4 &transform, const glm::mat4 &transformInv,
                            std::optional<float> size, ModelSceneNode *cullRoot) {
    ++_counts.entries;
    ++_counts.billboards;
    Material material {};
    material.type = MaterialType::Particle;
    material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)] = &texture;
    ProceduralInstance instance;
    instance.position = glm::vec3(transform[3]);
    instance.size = {glm::length(glm::vec3(transform[0])),
                     glm::length(glm::vec3(transform[1]))};
    instance.color = color;
    RegisteredProcedural procedural;
    procedural.categories = categories;
    procedural.id = id;
    procedural.nameIds = nameIds;
    procedural.material = material;
    procedural.kind = ProceduralKind::Billboard;
    procedural.instances.push_back(instance);
    procedural.cullRoot = cullRoot;
    _objects.push_back(std::move(procedural));
    (void)transformInv;
    (void)size;
}

void GpuScene::addParticles(RenderCategories categories, SceneNodeId id,
                            SceneNodeNameIds nameIds, const Material &material,
                            const glm::ivec2 &gridSize,
                            const std::vector<ParticleInstance> &instances,
                            ModelSceneNode *cullRoot) {
    ++_counts.entries;
    ++_counts.particleEmitters;
    _counts.particles += instances.size();
    RegisteredProcedural procedural;
    procedural.categories = categories;
    procedural.id = id;
    procedural.nameIds = nameIds;
    procedural.material = material;
    procedural.kind = ProceduralKind::Particles;
    procedural.gridSize = gridSize;
    procedural.cullRoot = cullRoot;
    procedural.instances.reserve(instances.size());
    for (const auto &source : instances) {
        ProceduralInstance instance;
        instance.variant = source.frame;
        instance.position = source.position;
        instance.size = source.size;
        instance.color = source.color;
        instance.right = source.right;
        instance.up = source.up;
        procedural.instances.push_back(instance);
    }
    _objects.push_back(std::move(procedural));
}

void GpuScene::addGrass(RenderCategories categories, SceneNodeId id,
                        SceneNodeNameIds nameIds, const Material &material, float radius,
                        float quadSize, const std::vector<GrassInstance> &instances) {
    ++_counts.entries;
    ++_counts.grassNodes;
    _counts.grassClusters += instances.size();
    RegisteredProcedural procedural;
    procedural.categories = categories;
    procedural.id = id;
    procedural.nameIds = nameIds;
    procedural.material = material;
    procedural.kind = ProceduralKind::Grass;
    procedural.quadSize = quadSize;
    procedural.instances.reserve(instances.size());
    for (const auto &source : instances) {
        ProceduralInstance instance;
        instance.variant = source.variant;
        instance.position = source.position;
        instance.lightmapUV = source.lightmapUV;
        instance.yaw = source.yaw;
        procedural.instances.push_back(instance);
    }
    _objects.push_back(std::move(procedural));
    (void)radius;
}

graphics::GpuSceneUpload GpuScene::prepare(
    const Classifier &classifier,
    const ProceduralClassifier &proceduralClassifier,
    const glm::mat4 &cameraView) const {
    graphics::GpuSceneUpload upload;
    std::vector<graphics::GpuSceneObjectInput> opaqueObjects, nonOpaqueObjects;
    upload.materials.reserve(_objects.size());
    opaqueObjects.reserve(_objects.size());
    nonOpaqueObjects.reserve(_objects.size());
    MaterialPool materials(upload);
    // ROWS, not columns. glm is column-major, so cameraView[i] is column i.
    // Billboards still use the primary-camera approximation; grass does not.
    const glm::mat3 viewRows = glm::transpose(glm::mat3(cameraView));
    const glm::vec3 viewRow0 = viewRows[0];
    const glm::vec3 viewRow1 = viewRows[1];

    // Retain the established merge order while using one procedural record and
    // lowering path: meshes and grass first, then particles, then billboards.
    // Absolute primitive order affects exact-tie traversal and accumulation.
    std::vector<const ObjectRecord *> orderedObjects;
    orderedObjects.reserve(_objects.size());
    for (const auto &object : _objects) {
        const auto *procedural = std::get_if<RegisteredProcedural>(&object);
        if (!procedural || procedural->kind == ProceduralKind::Grass)
            orderedObjects.push_back(&object);
    }
    for (const auto kind : {ProceduralKind::Particles, ProceduralKind::Billboard}) {
        for (const auto &object : _objects) {
            const auto *procedural = std::get_if<RegisteredProcedural>(&object);
            if (procedural && procedural->kind == kind)
                orderedObjects.push_back(&object);
        }
    }

    for (const auto *objectRecord : orderedObjects) {
        const auto &object = *objectRecord;
        const auto *mesh = std::get_if<RegisteredMesh>(&object);
        if (mesh) {
            if (!isObjectEnabled(mesh->id.index))
                continue;
            if ((mesh->categories & (renderCategory(RenderCategory::Opaque) | renderCategory(RenderCategory::Transparent))) == 0)
                continue;
            auto classification = classifier(*mesh);
            if (!classification)
                continue;
            graphics::GpuSceneObjectInput input;
            auto &sceneObject = input.data;
            input.sourceMesh = &mesh->mesh.get();
            input.objectIndex = mesh->id.index;
            input.objectGeneration = mesh->id.generation;
            sceneObject.transform = mesh->transform;
            sceneObject.prevTransform = mesh->prevTransform;
            sceneObject.transformInv = mesh->transformInv;
            const auto primitiveClass = applyAdmissionKind(*classification);
            sceneObject.materialIndex = materials.add(classification->material);
            if (classification->skin) {
                const auto &skin = *classification->skin;
                if (skin.bones.size() != skin.prevBones.size())
                    throw std::runtime_error("Vulkan: skinned mesh has mismatched bone palettes");
                if (upload.bones.size() + skin.bones.size() + skin.prevBones.size() >
                    std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Vulkan: merged scene exceeds shader index range");
                sceneObject.boneBase = static_cast<uint32_t>(upload.bones.size());
                sceneObject.boneCount = static_cast<uint32_t>(skin.bones.size());
                for (const auto &bone : skin.bones)
                    upload.bones.push_back(matrix3x4(bone));
                for (const auto &bone : skin.prevBones)
                    upload.bones.push_back(matrix3x4(bone));
            }
            if (const auto *dangly = std::get_if<RegisteredDangly>(&mesh->deformation)) {
                const auto vertexCount = mesh->mesh.get().vertexCount();
                if (dangly->positions.size() != vertexCount ||
                    dangly->prevPositions.size() != vertexCount)
                    throw std::runtime_error("Vulkan: dangly mesh has mismatched position streams");
                if (upload.danglyPositions.size() + dangly->positions.size() +
                        dangly->prevPositions.size() >
                    std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Vulkan: merged dangly position pool exceeds shader index range");
                sceneObject.danglyBase = static_cast<uint32_t>(upload.danglyPositions.size());
                sceneObject.danglyCount = static_cast<uint32_t>(vertexCount);
                upload.danglyPositions.insert(upload.danglyPositions.end(), dangly->positions.begin(),
                                              dangly->positions.end());
                upload.danglyPositions.insert(upload.danglyPositions.end(), dangly->prevPositions.begin(),
                                              dangly->prevPositions.end());
            }
            if (const auto *saber = std::get_if<RegisteredSaber>(&mesh->deformation)) {
                sceneObject.saberDisplacement = saber->displacement;
            }
            sceneObject.geometryIndex = primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
            if (sceneObject.geometryIndex == 0) {
                opaqueObjects.push_back(input);
            } else {
                nonOpaqueObjects.push_back(input);
            }
            continue;
        }
        const auto *procedural = std::get_if<RegisteredProcedural>(&object);
        if (!procedural || !isObjectEnabled(procedural->id.index) || procedural->instances.empty())
            continue;
        if ((procedural->categories & (renderCategory(RenderCategory::Opaque) |
                                       renderCategory(RenderCategory::Transparent))) == 0)
            continue;
        auto classification = proceduralClassifier(*procedural);
        if (!classification)
            continue;
        if (procedural->instances.size() > std::numeric_limits<uint32_t>::max() / 4 ||
            upload.proceduralQuads.size() >
                std::numeric_limits<uint32_t>::max() - procedural->instances.size())
            throw std::runtime_error("Vulkan: merged procedural scene exceeds shader index range");
        graphics::GpuSceneObjectInput input;
        auto &sceneObject = input.data;
        input.objectIndex = procedural->id.index;
        input.objectGeneration = procedural->id.generation;
        sceneObject.srcVertexOffset = static_cast<uint32_t>(upload.proceduralQuads.size());
        sceneObject.srcVertexStride = procedural->kind == ProceduralKind::Grass ? 0 : 1;
        sceneObject.vertexCount = static_cast<uint32_t>(procedural->instances.size()) * 4;
        sceneObject.triangleCount = static_cast<uint32_t>(procedural->instances.size()) * 2;
        const auto primitiveClass = applyAdmissionKind(*classification);
        sceneObject.materialIndex = materials.add(classification->material);
        sceneObject.geometryIndex = primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
        const glm::ivec2 grid = glm::max(procedural->gridSize, glm::ivec2(1));
        for (const auto &instance : procedural->instances) {
            graphics::GpuSceneProceduralQuad quad;
            quad.positionVariant = glm::vec4(instance.position,
                                             static_cast<float>(instance.variant));
            quad.color = instance.color;
            switch (procedural->kind) {
            case ProceduralKind::Grass: {
                // The same yaw raster receives in GrassUniforms. A blade stands
                // on world +Z and is independent of ray direction.
                const glm::vec3 right {glm::cos(instance.yaw) * procedural->quadSize,
                                       glm::sin(instance.yaw) * procedural->quadSize, 0.0f};
                quad.right = glm::vec4(right, 0.0f);
                quad.up = glm::vec4(0.0f, 0.0f, procedural->quadSize, 0.0f);
                quad.uvOffsetScale =
                    glm::vec4(0.5f * (instance.variant % 2),
                              0.5f * (instance.variant / 2), 0.5f, 0.5f);
                quad.lightmapUV = instance.lightmapUV;
                break;
            }
            case ProceduralKind::Particles: {
                const int frame = std::max(0, instance.variant);
                const glm::vec2 uvScale {1.0f / grid.x, 1.0f / grid.y};
                const glm::vec2 uvOffset {(frame % grid.x) * uvScale.x,
                                          (frame / grid.x) * uvScale.y};
                quad.positionVariant.w = static_cast<float>(frame);
                quad.right = glm::vec4(instance.right * instance.size.x, 0.0f);
                quad.up = glm::vec4(instance.up * instance.size.y, 0.0f);
                quad.uvOffsetScale = glm::vec4(uvOffset, uvScale);
                break;
            }
            case ProceduralKind::Billboard:
                // Billboard rasterization uses the primary-camera axes; the
                // merged quad fixes that same approximation for all ray types.
                quad.right = glm::vec4(viewRow0 * instance.size.x, 0.0f);
                quad.up = glm::vec4(viewRow1 * instance.size.y, 0.0f);
                break;
            }
            upload.proceduralQuads.push_back(quad);
        }
        if (sceneObject.geometryIndex == 0) {
            opaqueObjects.push_back(input);
        } else {
            nonOpaqueObjects.push_back(input);
        }
    }
    upload.opaqueObjectCount = static_cast<uint32_t>(opaqueObjects.size());
    upload.objects.reserve(opaqueObjects.size() + nonOpaqueObjects.size());
    upload.objects.insert(upload.objects.end(), opaqueObjects.begin(), opaqueObjects.end());
    upload.objects.insert(upload.objects.end(), nonOpaqueObjects.begin(), nonOpaqueObjects.end());
    return upload;
}
} // namespace reone::scene
