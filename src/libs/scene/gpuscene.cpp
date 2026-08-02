/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/scene/gpuscene.h"

#include <algorithm>
#include <limits>

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
    _objects.push_back(RegisteredBillboard {
        categories, id, nameIds, texture, color, transform, transformInv, size, cullRoot});
}

void GpuScene::addParticles(RenderCategories categories, SceneNodeId id,
                              SceneNodeNameIds nameIds, const Material &material,
                              const glm::ivec2 &gridSize,
                              const std::vector<ParticleInstance> &instances,
                              ModelSceneNode *cullRoot) {
    ++_counts.entries;
    ++_counts.particleEmitters;
    _counts.particles += instances.size();
    _objects.push_back(
        RegisteredParticles {categories, id, nameIds, material, gridSize, instances, cullRoot});
}

void GpuScene::addGrass(RenderCategories categories, SceneNodeId id,
                          SceneNodeNameIds nameIds, const Material &material, float radius,
                          float quadSize, const std::vector<GrassInstance> &instances) {
    ++_counts.entries;
    ++_counts.grassNodes;
    _counts.grassClusters += instances.size();
    _objects.push_back(
        RegisteredGrass {categories, id, nameIds, material, radius, quadSize, instances});
}

graphics::GpuSceneUpload GpuScene::prepare(
                                const Classifier &classifier,
                                const GrassClassifier &grassClassifier,
                                const ParticleClassifier &particleClassifier,
                                const BillboardClassifier &billboardClassifier,
                                const glm::mat4 &cameraView) const {
    graphics::GpuSceneUpload upload;
    std::vector<graphics::GpuSceneObjectInput> opaqueObjects, nonOpaqueObjects;
    upload.materials.reserve(_objects.size());
    opaqueObjects.reserve(_objects.size());
    nonOpaqueObjects.reserve(_objects.size());
    // ROWS, not columns. glm is column-major, so cameraView[i] is column i.
    // Billboards still use the primary-camera approximation; grass does not.
    const glm::mat3 viewRows = glm::transpose(glm::mat3(cameraView));
    const glm::vec3 viewRow0 = viewRows[0];
    const glm::vec3 viewRow1 = viewRows[1];
    const glm::vec3 viewRow2 = viewRows[2];
    for (const auto &object : _objects) {
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
            sceneObject.materialIndex = static_cast<uint32_t>(upload.materials.size());
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
            sceneObject.geometryIndex = classification->primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
            upload.materials.push_back(classification->material);
            if (sceneObject.geometryIndex == 0) {
                opaqueObjects.push_back(input);
            } else {
                nonOpaqueObjects.push_back(input);
            }
            continue;
        }
        const auto *grass = std::get_if<RegisteredGrass>(&object);
        if (!grass || !isObjectEnabled(grass->id.index) || grass->instances.empty())
            continue;
        if ((grass->categories & (renderCategory(RenderCategory::Opaque) | renderCategory(RenderCategory::Transparent))) == 0)
            continue;
        auto classification = grassClassifier(*grass);
        if (!classification)
            continue;
        if (grass->instances.size() > std::numeric_limits<uint32_t>::max() / 4 ||
            upload.proceduralQuads.size() >
                std::numeric_limits<uint32_t>::max() - grass->instances.size())
            throw std::runtime_error("Vulkan: merged grass scene exceeds shader index range");
        graphics::GpuSceneObjectInput input;
        auto &sceneObject = input.data;
        input.objectIndex = grass->id.index;
        input.objectGeneration = grass->id.generation;
        sceneObject.srcVertexOffset = static_cast<uint32_t>(upload.proceduralQuads.size());
        // A zero source stride tags a procedural grass-cluster list. The merge
        // shader expands it directly, avoiding one CPU SceneObject per blade.
        sceneObject.srcVertexStride = 0;
        sceneObject.vertexCount = static_cast<uint32_t>(grass->instances.size()) * 4;
        sceneObject.triangleCount = static_cast<uint32_t>(grass->instances.size()) * 2;
        sceneObject.materialIndex = static_cast<uint32_t>(upload.materials.size());
        sceneObject.geometryIndex = classification->primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
        for (const auto &instance : grass->instances) {
            // The same yaw raster receives in GrassUniforms. A blade stands on
            // world +Z and is invariant under primary, reflection and shadow
            // ray direction.
            const glm::vec3 right {glm::cos(instance.yaw) * grass->quadSize,
                                   glm::sin(instance.yaw) * grass->quadSize, 0.0f};
            const glm::vec3 up {0.0f, 0.0f, grass->quadSize};
            const glm::vec2 uvOffset {0.5f * (instance.variant % 2),
                                      0.5f * (instance.variant / 2)};
            upload.proceduralQuads.push_back(
                {glm::vec4(instance.position, static_cast<float>(instance.variant)),
                 glm::vec4(right, 0.0f), glm::vec4(up, 0.0f),
                 glm::vec4(uvOffset, glm::vec2(0.5f)), instance.lightmapUV,
                 {0.0f, 0.0f}, glm::vec4(1.0f)});
        }
        upload.materials.push_back(classification->material);
        if (sceneObject.geometryIndex == 0) {
            opaqueObjects.push_back(input);
        } else {
            nonOpaqueObjects.push_back(input);
        }
    }
    for (const auto &object : _objects) {
        const auto *particles = std::get_if<RegisteredParticles>(&object);
        if (!particles || !isObjectEnabled(particles->id.index) || particles->instances.empty())
            continue;
        if ((particles->categories & (renderCategory(RenderCategory::Opaque) |
                                      renderCategory(RenderCategory::Transparent))) == 0)
            continue;
        auto classification = particleClassifier(*particles);
        if (!classification)
            continue;
        if (particles->instances.size() > std::numeric_limits<uint32_t>::max() / 4 ||
            upload.proceduralQuads.size() >
                std::numeric_limits<uint32_t>::max() - particles->instances.size())
            throw std::runtime_error("Vulkan: merged particle scene exceeds shader index range");
        graphics::GpuSceneObjectInput input;
        auto &sceneObject = input.data;
        input.objectIndex = particles->id.index;
        input.objectGeneration = particles->id.generation;
        sceneObject.srcVertexOffset = static_cast<uint32_t>(upload.proceduralQuads.size());
        // One is the centered billboard tag. As with grass (zero), the merge
        // expands this compact emitter list into quads rather than receiving
        // one CPU SceneObject per particle.
        sceneObject.srcVertexStride = 1;
        sceneObject.vertexCount = static_cast<uint32_t>(particles->instances.size()) * 4;
        sceneObject.triangleCount = static_cast<uint32_t>(particles->instances.size()) * 2;
        sceneObject.materialIndex = static_cast<uint32_t>(upload.materials.size());
        sceneObject.geometryIndex = classification->primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
        const glm::ivec2 grid = glm::max(particles->gridSize, glm::ivec2(1));
        for (const auto &instance : particles->instances) {
            const int frame = std::max(0, instance.frame);
            const glm::vec2 uvScale {1.0f / grid.x, 1.0f / grid.y};
            const glm::vec2 uvOffset {(frame % grid.x) * uvScale.x, (frame / grid.x) * uvScale.y};
            upload.proceduralQuads.push_back(
                {glm::vec4(instance.position, static_cast<float>(frame)),
                 glm::vec4(instance.right * instance.size.x, 0.0f),
                 glm::vec4(instance.up * instance.size.y, 0.0f), glm::vec4(uvOffset, uvScale),
                 glm::vec2(0.0f), {0.0f, 0.0f}, instance.color});
        }
        upload.materials.push_back(classification->material);
        if (sceneObject.geometryIndex == 0) {
            opaqueObjects.push_back(input);
        } else {
            nonOpaqueObjects.push_back(input);
        }
    }
    for (const auto &object : _objects) {
        const auto *billboard = std::get_if<RegisteredBillboard>(&object);
        if (!billboard || !isObjectEnabled(billboard->id.index))
            continue;
        if ((billboard->categories & (renderCategory(RenderCategory::Opaque) |
                                      renderCategory(RenderCategory::Transparent))) == 0)
            continue;
        auto classification = billboardClassifier(*billboard);
        if (!classification)
            continue;
        graphics::GpuSceneObjectInput input;
        auto &sceneObject = input.data;
        input.objectIndex = billboard->id.index;
        input.objectGeneration = billboard->id.generation;
        sceneObject.srcVertexOffset = static_cast<uint32_t>(upload.proceduralQuads.size());
        sceneObject.srcVertexStride = 1;
        sceneObject.vertexCount = 4;
        sceneObject.triangleCount = 2;
        sceneObject.materialIndex = static_cast<uint32_t>(upload.materials.size());
        sceneObject.geometryIndex = classification->primitiveClass == PrimitiveClass::Opaque ? 0 : 1;
        // Billboard rasterization gets its axes from the primary camera. The
        // tracer bakes that same primary-camera approximation into the merged
        // quad; reflections and shadows therefore see a fixed pose.
        const float width = glm::length(glm::vec3(billboard->transform[0]));
        const float height = glm::length(glm::vec3(billboard->transform[1]));
        upload.proceduralQuads.push_back(
            {glm::vec4(glm::vec3(billboard->transform[3]), 0.0f),
             glm::vec4(viewRow0 * width, 0.0f), glm::vec4(viewRow1 * height, 0.0f),
             glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), glm::vec2(0.0f),
             {0.0f, 0.0f}, billboard->color});
        upload.materials.push_back(classification->material);
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
