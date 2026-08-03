/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <gtest/gtest.h>

#include "reone/scene/gpuscene.h"

using namespace reone::graphics;
using namespace reone::scene;

namespace {

std::vector<GpuSceneProceduralQuad> oneQuad(float x) {
    GpuSceneProceduralQuad quad;
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
