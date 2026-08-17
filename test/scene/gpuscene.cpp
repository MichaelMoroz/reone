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

void addParticles(reone::scene::GpuScene &scene, uint32_t index, float color = 1.0f) {
    Material material {};
    material.type = MaterialType::Particle;
    material.diffuseColor = glm::vec3(color);
    scene.addParticles(renderCategory(RenderCategory::Transparent), {index, 0}, {},
                       material, {1, 1}, oneQuad(static_cast<float>(index)), nullptr);
}

void addBillboard(reone::scene::GpuScene &scene, uint32_t index, Texture &texture,
                  float size = 1.0f) {
    scene.addBillboard(renderCategory(RenderCategory::Transparent), {index, 0}, {},
                       texture, glm::vec4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f),
                       size, nullptr);
}

reone::scene::GpuScene::Classifier noMeshes() {
    return [](const RegisteredMesh &) -> std::optional<reone::scene::GpuScene::Classification> {
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
    reone::scene::GpuScene scene;
    addParticles(scene, 5);
    addParticles(scene, 2);
    addParticles(scene, 3);

    int classifications = 0;
    auto classify = [&classifications](const RegisteredProcedural &object) {
        ++classifications;
        reone::scene::GpuScene::Classification result;
        result.kind = (object.id.index & 1u) != 0
                          ? reone::scene::GpuScene::AdmissionKind::Opaque
                          : reone::scene::GpuScene::AdmissionKind::LitBlended;
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
    reone::scene::GpuScene scene;
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

TEST(GpuScene, billboards_form_the_depth_independent_non_opaque_tail) {
    reone::scene::GpuScene scene;
    Texture texture("flare", TextureType::TwoDim, Texture::Properties {});
    addBillboard(scene, 1, texture);
    addParticles(scene, 2);

    auto classify = [](const RegisteredProcedural &object) {
        reone::scene::GpuScene::Classification result;
        result.kind = object.kind == ProceduralKind::Billboard
                          ? reone::scene::GpuScene::AdmissionKind::AdditiveEmissive
                          : reone::scene::GpuScene::AdmissionKind::LitBlended;
        return std::optional {result};
    };

    auto upload = scene.prepare(noMeshes(), classify, glm::mat4(1.0f), 1);
    ASSERT_EQ(2, upload.objects.size());
    EXPECT_EQ(0, upload.opaqueObjectCount);
    EXPECT_EQ(1, upload.depthIndependentObjectCount);
    EXPECT_EQ(2, upload.objects[0].objectIndex);
    EXPECT_EQ(1, upload.objects[1].objectIndex);
    EXPECT_EQ(2, upload.objects[1].data.triangleCount);
}

TEST(GpuScene, billboard_uses_authored_size_instead_of_translation_basis) {
    reone::scene::GpuScene scene;
    Texture texture("flare", TextureType::TwoDim, Texture::Properties {});
    addBillboard(scene, 1, texture, 3.0f);

    auto classify = [](const RegisteredProcedural &) {
        reone::scene::GpuScene::Classification result;
        result.kind = reone::scene::GpuScene::AdmissionKind::AdditiveEmissive;
        return std::optional {result};
    };

    auto upload = scene.prepare(noMeshes(), classify, glm::mat4(1.0f), 1);
    ASSERT_EQ(1, upload.proceduralQuads.size());
    EXPECT_FLOAT_EQ(3.0f, upload.proceduralQuads[0].right.x);
    EXPECT_FLOAT_EQ(3.0f, upload.proceduralQuads[0].up.y);
}

TEST(GpuScene, grass_uses_face_band_prefix_ranges_without_cpu_quads) {
    reone::scene::GpuScene scene;
    std::vector<GrassFace> faces {
        grassFace(7, 3, 0.0f), grassFace(11, 5, 20.0f),
        grassFace(19, 2, 100.0f)};
    Material material {};
    material.type = MaterialType::Grass;
    scene.addGrass(renderCategory(RenderCategory::Opaque), {4, 0}, {}, material,
                   faces, 1);

    // Pinned rather than left to the defaults, and deliberately not the default
    // values: the counts below are the blade arithmetic, and a test that reads
    // them out of whatever the defaults happen to be stops measuring it the
    // moment someone retunes the grass. Which is how this test came to assert
    // the old cardboard quad long after blades replaced it. The radius has to
    // keep the near camera in reach of the first two faces and out of reach of
    // the third, since selection is by distance.
    GrassParams params;
    params.radius = 32.0f;
    params.segments = 3;
    params.bladesPerCluster = 2;
    scene.setGrassParams(params);

    int classifications = 0;
    auto classify = [&classifications](const RegisteredProcedural &object) {
        ++classifications;
        EXPECT_EQ(ProceduralKind::Grass, object.kind);
        reone::scene::GpuScene::Classification result;
        result.kind = reone::scene::GpuScene::AdmissionKind::Cutout;
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
    // 8 granted clusters * 2 blades, each a strip of 3 quads closed by a tip:
    // 2n+3 = 9 vertices and 2n+1 = 7 triangles. Spelled as literals rather than
    // through grassVertsPerBlade, so an error in that helper cannot satisfy
    // both the recorder and its test.
    EXPECT_EQ(144, near.objects[0].data.vertexCount);
    EXPECT_EQ(112, near.objects[0].data.triangleCount);
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
    // Only the far face is in reach now: 2 clusters * 2 blades * 9 and * 7.
    EXPECT_EQ(36, far.objects[0].data.vertexCount);
    EXPECT_EQ(28, far.objects[0].data.triangleCount);

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
