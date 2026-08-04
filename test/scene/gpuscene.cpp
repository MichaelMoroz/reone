/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <gtest/gtest.h>

#include "reone/scene/gpuscene.h"

using namespace reone::graphics;
using namespace reone::scene;

namespace {

std::vector<ProceduralQuad> oneQuad(float x) {
    ProceduralQuad quad;
    quad.positionVariant.x = x;
    return {quad};
}

void addParticles(GpuScene &scene, uint32_t index, float color = 1.0f) {
    Material material {};
    material.type = MaterialType::Particle;
    material.diffuseColor = glm::vec3(color);
    scene.addParticles(renderCategory(RenderCategory::Transparent), {index, 0}, {},
                       material, {1, 1}, oneQuad(static_cast<float>(index)), nullptr);
}

GpuScene::Classifier noMeshes() {
    return [](const RegisteredMesh &) -> std::optional<GpuScene::Classification> {
        ADD_FAILURE() << "unexpected mesh";
        return std::nullopt;
    };
}

GrassFace grassFace(uint32_t sourceFace, uint32_t budget, float x) {
    GrassFace face;
    face.vertex0Uv0x = {x - 1.0f, -1.0f, 0.0f, 0.0f};
    face.vertex1Uv0y = {x + 1.0f, -1.0f, 0.0f, 0.0f};
    face.vertex2Uv1x = {x, 1.0f, 0.0f, 0.0f};
    face.uv1yUv2QuadSize.w = 0.5f;
    face.probabilities = {1.0f, 0.0f, 0.0f, 0.0f};
    face.boundsMin = {x - 1.0f, -1.0f, 0.0f, 0.0f};
    face.boundsMax = {x + 1.0f, 1.0f, 0.0f, 0.0f};
    face.faceBudgetMaterialVariants = {sourceFace, budget, 0u, 4u};
    return face;
}

} // namespace

TEST(GpuScene, persists_objects_and_caches_classification_in_canonical_order) {
    GpuScene scene;
    addParticles(scene, 5);
    addParticles(scene, 2);
    addParticles(scene, 3);

    int classifications = 0;
    auto classify = [&classifications](const RegisteredProcedural &object) {
        ++classifications;
        GpuScene::Classification result;
        result.kind = (object.id.index & 1u) != 0
                          ? GpuScene::AdmissionKind::Opaque
                          : GpuScene::AdmissionKind::LitBlended;
        result.material.diffuseColor =
            glm::vec4(object.material.diffuseColor, 1.0f);
        return std::optional {result};
    };

    auto first = scene.prepare(noMeshes(), classify, glm::mat4(1.0f), 1);
    ASSERT_EQ(3, first.objects.size());
    EXPECT_EQ(2, first.opaqueObjectCount);
    EXPECT_EQ(3, classifications);
    EXPECT_EQ(3, first.objects[0].objectIndex);
    EXPECT_EQ(5, first.objects[1].objectIndex);
    EXPECT_EQ(2, first.objects[2].objectIndex);

    const auto stableMaterial = first.objects[1].data.materialIndex;
    scene.resetFrame();
    auto second = scene.prepare(noMeshes(), classify, glm::mat4(1.0f), 1);
    EXPECT_EQ(3, classifications);
    EXPECT_EQ(stableMaterial, second.objects[1].data.materialIndex);

    addParticles(scene, 3, 0.5f);
    auto third = scene.prepare(noMeshes(), classify, glm::mat4(1.0f), 1);
    EXPECT_EQ(4, classifications);
    ASSERT_EQ(3, third.objects.size());
    EXPECT_EQ(stableMaterial, third.objects[1].data.materialIndex);
}

TEST(GpuScene, full_collection_unregisters_unseen_objects_and_clear_resets_world) {
    GpuScene scene;
    addParticles(scene, 1);
    addParticles(scene, 2);
    scene.beginFullCollection();
    addParticles(scene, 2);
    scene.endFullCollection();
    ASSERT_EQ(1, scene.objects().size());
    EXPECT_EQ(2, std::get<RegisteredProcedural>(scene.objects().front()).id.index);

    scene.clear();
    EXPECT_TRUE(scene.objects().empty());
    EXPECT_EQ(0, scene.counts().objects());
}

TEST(GpuScene, grass_uses_face_band_prefix_ranges_without_cpu_quads) {
    GpuScene scene;
    std::vector<GrassFace> faces {
        grassFace(7, 3, 0.0f), grassFace(11, 5, 20.0f),
        grassFace(19, 2, 100.0f)};
    Material material {};
    material.type = MaterialType::Grass;
    scene.addGrass(renderCategory(RenderCategory::Opaque), {4, 0}, {}, material,
                   faces, 1);

    int classifications = 0;
    auto classify = [&classifications](const RegisteredProcedural &object) {
        ++classifications;
        EXPECT_EQ(ProceduralKind::Grass, object.kind);
        GpuScene::Classification result;
        result.kind = GpuScene::AdmissionKind::Cutout;
        return std::optional {result};
    };

    auto near = scene.prepare(noMeshes(), classify, glm::mat4(1.0f), 1);
    ASSERT_EQ(1, near.objects.size());
    EXPECT_EQ(1, classifications);
    EXPECT_TRUE(near.proceduralQuads.empty());
    ASSERT_EQ(3, near.grassFaces.size());
    ASSERT_EQ(2, near.grassRanges.size());
    EXPECT_EQ(0, near.grassRanges[0].faceIndex);
    EXPECT_EQ(0, near.grassRanges[0].clusterOffset);
    EXPECT_EQ(3, near.grassRanges[0].clusterCount);
    EXPECT_EQ(1, near.grassRanges[1].faceIndex);
    EXPECT_EQ(3, near.grassRanges[1].clusterOffset);
    EXPECT_EQ(5, near.grassRanges[1].clusterCount);
    EXPECT_EQ(32, near.objects[0].data.vertexCount);
    EXPECT_EQ(16, near.objects[0].data.triangleCount);
    EXPECT_EQ(2, near.objects[0].data.srcIndexOffset);
    EXPECT_EQ(near.objects[0].data.materialIndex,
              near.grassFaces[0].faceBudgetMaterialVariants.z);

    const auto generation = near.grassFaceGeneration;
    auto farView = glm::translate(glm::mat4(1.0f), glm::vec3(-100.0f, 0.0f, 0.0f));
    auto far = scene.prepare(noMeshes(), classify, farView, 1, false,
                             std::move(near));
    EXPECT_EQ(1, classifications);
    EXPECT_EQ(generation, far.grassFaceGeneration);
    ASSERT_EQ(1, far.grassRanges.size());
    EXPECT_EQ(2, far.grassRanges[0].faceIndex);
    EXPECT_EQ(0, far.grassRanges[0].clusterOffset);
    EXPECT_EQ(2, far.grassRanges[0].clusterCount);
    ASSERT_EQ(1, far.objects.size());
    EXPECT_EQ(8, far.objects[0].data.vertexCount);
    EXPECT_EQ(4, far.objects[0].data.triangleCount);

    faces[0].faceBudgetMaterialVariants.y = 9;
    scene.addGrass(renderCategory(RenderCategory::Opaque), {4, 0}, {}, material,
                   faces, 2);
    auto denser = scene.prepare(noMeshes(), classify, glm::mat4(1.0f), 1,
                                false, std::move(far));
    EXPECT_NE(generation, denser.grassFaceGeneration);
    EXPECT_EQ(16, scene.counts().grassClusters);
    ASSERT_EQ(2, denser.grassRanges.size());
    EXPECT_EQ(0, denser.grassRanges[0].clusterOffset);
    EXPECT_EQ(9, denser.grassRanges[0].clusterCount);
    EXPECT_EQ(9, denser.grassRanges[1].clusterOffset);
}
