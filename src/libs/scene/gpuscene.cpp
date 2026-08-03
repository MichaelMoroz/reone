/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/scene/gpuscene.h"

#include "reone/system/profiler.h"

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

bool sameMaterial(const Material &left, const Material &right) {
    return left.type == right.type && left.textures == right.textures &&
           std::memcmp(&left.uv, &right.uv, sizeof(left.uv)) == 0 &&
           std::memcmp(&left.color, &right.color, sizeof(left.color)) == 0 &&
           left.bumpMapFrame == right.bumpMapFrame &&
           std::memcmp(&left.ambientColor, &right.ambientColor,
                       sizeof(left.ambientColor)) == 0 &&
           std::memcmp(&left.diffuseColor, &right.diffuseColor,
                       sizeof(left.diffuseColor)) == 0 &&
           std::memcmp(&left.selfIllumColor, &right.selfIllumColor,
                       sizeof(left.selfIllumColor)) == 0 &&
           left.staticObject == right.staticObject &&
           left.backgroundGeometry == right.backgroundGeometry &&
           left.curatedIndex == right.curatedIndex &&
           left.affectedByShadows == right.affectedByShadows &&
           left.affectedByFog == right.affectedByFog &&
           left.blending == right.blending && left.faceCulling == right.faceCulling &&
           left.polygonMode == right.polygonMode;
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
    // Persistent object and stream records are updated at their simulation
    // lifecycle boundaries. Nothing in the world table is frame-owned now.
}

void GpuScene::beginFullCollection() {
    _fullCollectionUnseen.clear();
    for (const auto &object : _objects)
        _fullCollectionUnseen.insert(objectId(object));
}

void GpuScene::endFullCollection() {
    for (const auto id : _fullCollectionUnseen)
        unregisterObject(id);
    _fullCollectionUnseen.clear();
}

void GpuScene::clear() {
    _objects.clear();
    resetAdmissionCache();
    _inactiveObjects.clear();
    _fullCollectionUnseen.clear();
    _disabledObjects.clear();
    _counts = {};
    _previousFrameIds.clear();
    ++_admissionGeneration;
}

void GpuScene::resetAdmissionCache() {
    _classification.clear();
    _materials.clear();
    _materialIndices.clear();
    _freeMaterialIndices.clear();
}

SceneNodeId GpuScene::objectId(const ObjectRecord &object) {
    return std::visit([](const auto &entry) { return entry.id; }, object);
}

bool GpuScene::idLess(SceneNodeId left, SceneNodeId right) {
    return left.index != right.index ? left.index < right.index
                                     : left.generation < right.generation;
}

std::vector<ObjectRecord>::iterator GpuScene::findObject(SceneNodeId id) {
    auto it = std::lower_bound(_objects.begin(), _objects.end(), id,
                               [](const ObjectRecord &object, SceneNodeId value) {
                                   return idLess(objectId(object), value);
                               });
    return it != _objects.end() && objectId(*it) == id ? it : _objects.end();
}

std::vector<ObjectRecord>::const_iterator GpuScene::findObject(SceneNodeId id) const {
    auto it = std::lower_bound(_objects.begin(), _objects.end(), id,
                               [](const ObjectRecord &object, SceneNodeId value) {
                                   return idLess(objectId(object), value);
                               });
    return it != _objects.end() && objectId(*it) == id ? it : _objects.end();
}

void GpuScene::addCounts(const ObjectRecord &object) {
    ++_counts.entries;
    if (const auto *mesh = std::get_if<RegisteredMesh>(&object)) {
        if (isCountedMesh(mesh->material))
            countMesh(_counts, mesh->deformation);
        return;
    }
    const auto &procedural = std::get<RegisteredProcedural>(object);
    switch (procedural.kind) {
    case ProceduralKind::Grass:
        ++_counts.grassNodes;
        _counts.grassClusters += procedural.instanceCount();
        break;
    case ProceduralKind::Particles:
        ++_counts.particleEmitters;
        _counts.particles += procedural.instanceCount();
        break;
    case ProceduralKind::Billboard:
        ++_counts.billboards;
        break;
    }
}

void GpuScene::removeCounts(const ObjectRecord &object) {
    --_counts.entries;
    if (const auto *mesh = std::get_if<RegisteredMesh>(&object)) {
        if (!isCountedMesh(mesh->material))
            return;
        if (std::holds_alternative<RegisteredSkin>(mesh->deformation))
            --_counts.skinned;
        else if (std::holds_alternative<RegisteredDangly>(mesh->deformation))
            --_counts.dangly;
        else if (std::holds_alternative<RegisteredSaber>(mesh->deformation))
            --_counts.saber;
        else
            --_counts.rigid;
        return;
    }
    const auto &procedural = std::get<RegisteredProcedural>(object);
    switch (procedural.kind) {
    case ProceduralKind::Grass:
        --_counts.grassNodes;
        _counts.grassClusters -= procedural.instanceCount();
        break;
    case ProceduralKind::Particles:
        --_counts.particleEmitters;
        _counts.particles -= procedural.instanceCount();
        break;
    case ProceduralKind::Billboard:
        --_counts.billboards;
        break;
    }
}

void GpuScene::releaseMaterial(uint32_t index) {
    if (index >= _materials.size() || !_materials[index].occupied)
        return;
    auto &entry = _materials[index];
    if (--entry.references == 0) {
        entry.occupied = false;
        _freeMaterialIndices.push_back(index);
    }
}

uint32_t GpuScene::internMaterial(const InstanceMaterial &material) {
    const auto hash = hashBytes(&material, sizeof(material));
    auto &candidates = _materialIndices[hash];
    for (const auto index : candidates) {
        const auto &entry = _materials[index];
        if (entry.occupied &&
            std::memcmp(&entry.material, &material, sizeof(material)) == 0) {
            ++_materials[index].references;
            return index;
        }
    }
    uint32_t index;
    if (_freeMaterialIndices.empty()) {
        index = static_cast<uint32_t>(_materials.size());
        _materials.push_back({});
    } else {
        index = _freeMaterialIndices.back();
        _freeMaterialIndices.pop_back();
    }
    _materials[index] = {material, 1, hash, true};
    candidates.push_back(index);
    return index;
}

void GpuScene::invalidate(SceneNodeId id) {
    auto &cache = _classification[id];
    cache.dirty = true;
}

void GpuScene::upsert(ObjectRecord object) {
    const auto id = objectId(object);
    _fullCollectionUnseen.erase(id);
    if (!isActive(id))
        return;
    auto it = findObject(id);
    if (it == _objects.end()) {
        auto insertion = std::lower_bound(
            _objects.begin(), _objects.end(), id,
            [](const ObjectRecord &candidate, SceneNodeId value) {
                return idLess(objectId(candidate), value);
            });
        addCounts(object);
        _objects.insert(insertion, std::move(object));
        _classification.try_emplace(id);
        return;
    }

    bool classificationChanged = it->index() != object.index();
    if (!classificationChanged) {
        if (const auto *oldMesh = std::get_if<RegisteredMesh>(&*it)) {
            const auto &newMesh = std::get<RegisteredMesh>(object);
            classificationChanged =
                oldMesh->categories != newMesh.categories ||
                &oldMesh->mesh.get() != &newMesh.mesh.get() ||
                oldMesh->cullRoot != newMesh.cullRoot ||
                oldMesh->deformation.index() != newMesh.deformation.index() ||
                !sameMaterial(oldMesh->material, newMesh.material);
        } else {
            const auto &oldProcedural = std::get<RegisteredProcedural>(*it);
            const auto &newProcedural = std::get<RegisteredProcedural>(object);
            classificationChanged =
                oldProcedural.categories != newProcedural.categories ||
                oldProcedural.kind != newProcedural.kind ||
                oldProcedural.cullRoot != newProcedural.cullRoot ||
                !sameMaterial(oldProcedural.material, newProcedural.material);
            if (!classificationChanged && newProcedural.kind == ProceduralKind::Billboard &&
                !oldProcedural.instances.empty() && !newProcedural.instances.empty()) {
                classificationChanged =
                    std::memcmp(&oldProcedural.instances.front().color,
                                &newProcedural.instances.front().color,
                                sizeof(glm::vec4)) != 0;
            }
        }
    }
    removeCounts(*it);
    *it = std::move(object);
    addCounts(*it);
    if (classificationChanged)
        invalidate(id);
}

void GpuScene::eraseObject(std::vector<ObjectRecord>::iterator it) {
    const auto id = objectId(*it);
    removeCounts(*it);
    auto cache = _classification.find(id);
    if (cache != _classification.end()) {
        if (cache->second.classified && cache->second.value)
            releaseMaterial(cache->second.materialIndex);
        _classification.erase(cache);
    }
    _objects.erase(it);
}

void GpuScene::unregisterObject(SceneNodeId id) {
    auto it = findObject(id);
    if (it != _objects.end())
        eraseObject(it);
}

void GpuScene::setObjectActive(SceneNodeId id, bool active) {
    if (active) {
        _inactiveObjects.erase(id);
    } else {
        _inactiveObjects.insert(id);
        unregisterObject(id);
    }
    if (_shadowScene)
        _shadowScene->setObjectActive(id, active);
}

bool GpuScene::isActive(SceneNodeId id) const {
    return _inactiveObjects.find(id) == _inactiveObjects.end();
}

void GpuScene::updateMeshTransform(SceneNodeId id, const glm::mat4 &transform,
                                   const glm::mat4 &transformInv,
                                   const glm::mat4 &prevTransform) {
    auto it = findObject(id);
    if (it == _objects.end())
        return;
    if (auto *mesh = std::get_if<RegisteredMesh>(&*it)) {
        mesh->transform = transform;
        mesh->transformInv = transformInv;
        mesh->prevTransform = prevTransform;
    }
}

void GpuScene::settleMeshTransform(SceneNodeId id, const glm::mat4 &transform) {
    auto it = findObject(id);
    if (it == _objects.end())
        return;
    if (auto *mesh = std::get_if<RegisteredMesh>(&*it))
        mesh->prevTransform = transform;
}

bool GpuScene::updateMeshDeformation(SceneNodeId id,
                                     RegisteredDeformation deformation) {
    auto it = findObject(id);
    if (it == _objects.end())
        return false;
    auto *mesh = std::get_if<RegisteredMesh>(&*it);
    if (!mesh)
        return false;
    if (mesh->deformation.index() != deformation.index())
        invalidate(id);
    mesh->deformation = std::move(deformation);
    return true;
}

void GpuScene::patchBumpMapFrame(SceneNodeId id, int frame) {
    auto it = findObject(id);
    if (it == _objects.end())
        return;
    auto *mesh = std::get_if<RegisteredMesh>(&*it);
    if (!mesh || mesh->material.bumpMapFrame == frame)
        return;
    mesh->material.bumpMapFrame = frame;

    auto cacheIt = _classification.find(id);
    if (cacheIt == _classification.end())
        return;
    auto &cache = cacheIt->second;
    if (cache.dirty || !cache.classified || !cache.value)
        return;
    if (cache.value->material.bumpMapFrame == frame)
        return;

    releaseMaterial(cache.materialIndex);
    cache.value->material.bumpMapFrame = frame;
    cache.materialIndex = internMaterial(cache.value->material);
}

void GpuScene::patchMaterialUv(SceneNodeId id, const glm::mat3x4 &uv) {
    auto it = findObject(id);
    if (it == _objects.end())
        return;
    auto *mesh = std::get_if<RegisteredMesh>(&*it);
    if (!mesh || std::memcmp(&mesh->material.uv, &uv, sizeof(uv)) == 0)
        return;
    mesh->material.uv = uv;

    auto cacheIt = _classification.find(id);
    if (cacheIt == _classification.end())
        return;
    auto &cache = cacheIt->second;
    if (cache.dirty || !cache.classified || !cache.value)
        return;

    releaseMaterial(cache.materialIndex);
    cache.value->material.uv0 = uv[0];
    cache.value->material.uv1 = uv[1];
    cache.value->material.uv2 = uv[2];
    cache.materialIndex = internMaterial(cache.value->material);
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
    upsert(RegisteredMesh {categories, id, nameIds, mesh, material, transform,
                           transformInv, prevTransform, std::move(deformation), cullRoot});
}

void GpuScene::addBillboard(RenderCategories categories, SceneNodeId id,
                            SceneNodeNameIds nameIds, Texture &texture, const glm::vec4 &color,
                            const glm::mat4 &transform, const glm::mat4 &transformInv,
                            std::optional<float> size, ModelSceneNode *cullRoot) {
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
    upsert(std::move(procedural));
    (void)transformInv;
    (void)size;
}

void GpuScene::addParticles(RenderCategories categories, SceneNodeId id,
                            SceneNodeNameIds nameIds, const Material &material,
                            const glm::ivec2 &gridSize,
                            std::vector<graphics::GpuSceneProceduralQuad> quads,
                            ModelSceneNode *cullRoot) {
    RegisteredProcedural procedural;
    procedural.categories = categories;
    procedural.id = id;
    procedural.nameIds = nameIds;
    procedural.material = material;
    procedural.kind = ProceduralKind::Particles;
    procedural.gridSize = gridSize;
    procedural.cullRoot = cullRoot;
    procedural.loweredQuads = std::move(quads);
    upsert(std::move(procedural));
}

void GpuScene::addGrass(RenderCategories categories, SceneNodeId id,
                        SceneNodeNameIds nameIds, const Material &material, float radius,
                        float quadSize, const std::vector<GrassInstance> &instances) {
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
        graphics::GpuSceneProceduralQuad quad;
        quad.positionVariant = glm::vec4(source.position,
                                         static_cast<float>(source.variant));
        const glm::vec3 right {glm::cos(source.yaw) * quadSize,
                               glm::sin(source.yaw) * quadSize, 0.0f};
        quad.right = glm::vec4(right, 0.0f);
        quad.up = glm::vec4(0.0f, 0.0f, quadSize, 0.0f);
        quad.uvOffsetScale =
            glm::vec4(0.5f * (source.variant % 2),
                      0.5f * (source.variant / 2), 0.5f, 0.5f);
        quad.lightmapUV = source.lightmapUV;
        procedural.loweredQuads.push_back(quad);
    }
    upsert(std::move(procedural));
    (void)radius;
}

graphics::GpuSceneUpload GpuScene::prepare(
    const Classifier &classifier,
    const ProceduralClassifier &proceduralClassifier,
    const glm::mat4 &cameraView,
    uint64_t admissionGeneration,
    bool forceFull,
    graphics::GpuSceneUpload reuse) {
    R_PROFILE_ZONE("SceneAdmission::classification + dedup");
    auto upload = std::move(reuse);
    upload.materials.clear();
    upload.objects.clear();
    upload.bones.clear();
    upload.danglyPositions.clear();
    upload.proceduralQuads.clear();
    upload.opaqueObjectCount = 0;
    upload.materialReferenceCount = 0;
    std::vector<graphics::GpuSceneObjectInput> opaqueObjects, nonOpaqueObjects;
    opaqueObjects.reserve(_objects.size());
    nonOpaqueObjects.reserve(_objects.size());
    // ROWS, not columns. glm is column-major, so cameraView[i] is column i.
    // Billboards still use the primary-camera approximation; grass does not.
    const glm::mat3 viewRows = glm::transpose(glm::mat3(cameraView));
    const glm::vec3 viewRow0 = viewRows[0];
    const glm::vec3 viewRow1 = viewRows[1];

    // _objects is maintained in canonical SceneNodeId order. Partitioning into
    // these two vectors therefore yields opaque-first, stable-id order without
    // a per-frame sort.
    for (const auto &object : _objects) {
        const auto id = objectId(object);
        if (!isActive(id))
            continue;
        const auto *mesh = std::get_if<RegisteredMesh>(&object);
        if (mesh) {
            if (!isObjectEnabled(mesh->id.index))
                continue;
            if ((mesh->categories & (renderCategory(RenderCategory::Opaque) | renderCategory(RenderCategory::Transparent))) == 0)
                continue;
            auto &cache = _classification[id];
            if (forceFull || cache.dirty || !cache.classified ||
                cache.admissionGeneration != admissionGeneration) {
                if (cache.classified && cache.value)
                    releaseMaterial(cache.materialIndex);
                cache.value = classifier(*mesh);
                cache.classified = true;
                cache.dirty = false;
                cache.admissionGeneration = admissionGeneration;
                if (cache.value) {
                    applyAdmissionKind(*cache.value);
                    cache.materialIndex = internMaterial(cache.value->material);
                }
            }
            if (!cache.value)
                continue;
            graphics::GpuSceneObjectInput input;
            auto &sceneObject = input.data;
            input.sourceMesh = &mesh->mesh.get();
            input.objectIndex = mesh->id.index;
            input.objectGeneration = mesh->id.generation;
            sceneObject.transform = mesh->transform;
            sceneObject.prevTransform = mesh->prevTransform;
            sceneObject.transformInv = mesh->transformInv;
            const auto primitiveClass =
                cache.value->kind == AdmissionKind::Opaque
                    ? PrimitiveClass::Opaque
                    : PrimitiveClass::NonOpaque;
            sceneObject.materialIndex = cache.materialIndex;
            ++upload.materialReferenceCount;
            if (const auto *skinPtr = std::get_if<RegisteredSkin>(&mesh->deformation)) {
                const auto &skin = *skinPtr;
                if (!skin.bones || !skin.prevBones ||
                    skin.bones->size() != skin.prevBones->size())
                    throw std::runtime_error("Vulkan: skinned mesh has mismatched bone palettes");
                if (upload.bones.size() + skin.bones->size() + skin.prevBones->size() >
                    std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Vulkan: merged scene exceeds shader index range");
                sceneObject.boneBase = static_cast<uint32_t>(upload.bones.size());
                sceneObject.boneCount = static_cast<uint32_t>(skin.bones->size());
                for (const auto &bone : *skin.bones)
                    upload.bones.push_back(matrix3x4(bone));
                for (const auto &bone : *skin.prevBones)
                    upload.bones.push_back(matrix3x4(bone));
            }
            if (const auto *dangly = std::get_if<RegisteredDangly>(&mesh->deformation)) {
                const auto vertexCount = mesh->mesh.get().vertexCount();
                if (!dangly->positions || !dangly->prevPositions ||
                    dangly->positions->size() != vertexCount ||
                    dangly->prevPositions->size() != vertexCount)
                    throw std::runtime_error("Vulkan: dangly mesh has mismatched position streams");
                if (upload.danglyPositions.size() + dangly->positions->size() +
                        dangly->prevPositions->size() >
                    std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Vulkan: merged dangly position pool exceeds shader index range");
                sceneObject.danglyBase = static_cast<uint32_t>(upload.danglyPositions.size());
                sceneObject.danglyCount = static_cast<uint32_t>(vertexCount);
                upload.danglyPositions.insert(upload.danglyPositions.end(),
                                              dangly->positions->begin(),
                                              dangly->positions->end());
                upload.danglyPositions.insert(upload.danglyPositions.end(),
                                              dangly->prevPositions->begin(),
                                              dangly->prevPositions->end());
            }
            if (const auto *saber = std::get_if<RegisteredSaber>(&mesh->deformation)) {
                if (saber->displacement)
                    sceneObject.saberDisplacement =
                        glm::vec4(*saber->displacement, 0.0f);
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
        if (!procedural || !isObjectEnabled(procedural->id.index) ||
            procedural->instanceCount() == 0)
            continue;
        if ((procedural->categories & (renderCategory(RenderCategory::Opaque) |
                                       renderCategory(RenderCategory::Transparent))) == 0)
            continue;
        R_PROFILE_ZONE("SceneAdmission::procedural lowering");
        auto &cache = _classification[id];
        if (forceFull || cache.dirty || !cache.classified ||
            cache.admissionGeneration != admissionGeneration) {
            if (cache.classified && cache.value)
                releaseMaterial(cache.materialIndex);
            cache.value = proceduralClassifier(*procedural);
            cache.classified = true;
            cache.dirty = false;
            cache.admissionGeneration = admissionGeneration;
            if (cache.value) {
                applyAdmissionKind(*cache.value);
                cache.materialIndex = internMaterial(cache.value->material);
            }
        }
        if (!cache.value)
            continue;
        const auto instanceCount = procedural->instanceCount();
        if (instanceCount > std::numeric_limits<uint32_t>::max() / 4 ||
            upload.proceduralQuads.size() >
                std::numeric_limits<uint32_t>::max() - instanceCount)
            throw std::runtime_error("Vulkan: merged procedural scene exceeds shader index range");
        graphics::GpuSceneObjectInput input;
        auto &sceneObject = input.data;
        input.objectIndex = procedural->id.index;
        input.objectGeneration = procedural->id.generation;
        sceneObject.srcVertexOffset = static_cast<uint32_t>(upload.proceduralQuads.size());
        sceneObject.srcVertexStride = procedural->kind == ProceduralKind::Grass ? 0 : 1;
        sceneObject.vertexCount = static_cast<uint32_t>(instanceCount) * 4;
        sceneObject.triangleCount = static_cast<uint32_t>(instanceCount) * 2;
        const auto primitiveClass =
            cache.value->kind == AdmissionKind::Opaque
                ? PrimitiveClass::Opaque
                : PrimitiveClass::NonOpaque;
        sceneObject.materialIndex = cache.materialIndex;
        ++upload.materialReferenceCount;
        sceneObject.geometryIndex = primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
        const glm::ivec2 grid = glm::max(procedural->gridSize, glm::ivec2(1));
        if (!procedural->loweredQuads.empty() &&
            procedural->loweredQuads.size() == instanceCount) {
            upload.proceduralQuads.insert(upload.proceduralQuads.end(),
                                          procedural->loweredQuads.begin(),
                                          procedural->loweredQuads.end());
        } else for (const auto &instance : procedural->instances) {
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
    upload.materials.assign(_materials.size(), {});
    for (size_t i = 0; i < _materials.size(); ++i) {
        if (_materials[i].occupied)
            upload.materials[i] = _materials[i].material;
    }
    return upload;
}
} // namespace reone::scene
