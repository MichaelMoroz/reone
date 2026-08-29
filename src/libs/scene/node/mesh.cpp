/*
 * Copyright (c) 2020-2023 The reone project contributors
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

#include "reone/scene/node/mesh.h"

#include "reone/graphics/di/services.h"
#include "reone/graphics/lumautil.h"
#include "reone/graphics/material.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/textureutil.h"
#include "reone/graphics/uniforms.h"
#include "reone/resource/di/services.h"
#include "reone/resource/provider/textures.h"
#include "reone/scene/gpuscene.h"
#include "reone/scene/graph.h"
#include "reone/scene/node/camera.h"
#include "reone/scene/node/light.h"
#include "reone/scene/node/model.h"
#include "reone/scene/render/pipeline.h"
#include "reone/system/logutil.h"
#include "reone/system/profiler.h"
#include "reone/system/randomutil.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

static constexpr float kUvAnimationSpeed = 250.0f;

void MeshSceneNode::init() {
    _point = false;
    _modelNode.floatValueAtTime(ControllerTypes::alpha, 0.0f, _alpha);
    _modelNode.vectorValueAtTime(ControllerTypes::selfIllumColor, 0.0f, _selfIllumColor);

    initTextures();
    initDanglyMesh();
}

void MeshSceneNode::initTextures() {
    std::shared_ptr<ModelNode::TriangleMesh> mesh(_modelNode.mesh());
    if (!mesh) {
        return;
    }
    if (!mesh->diffuseMap.empty()) {
        auto diffuseMap = _resourceSvc.textures.get(mesh->diffuseMap, TextureUsage::MainTex);
        _nodeTextures.diffuse = diffuseMap.get();
    }
    if (!mesh->lightmap.empty()) {
        auto lightmap = _resourceSvc.textures.get(mesh->lightmap, TextureUsage::Lightmap);
        _nodeTextures.lightmap = lightmap.get();
    }
    if (!mesh->bumpmap.empty()) {
        auto bumpmap = _resourceSvc.textures.get(mesh->bumpmap, TextureUsage::BumpMap);
        _nodeTextures.bumpmap = bumpmap.get();
    }
    refreshAdditionalTextures();
}

void MeshSceneNode::refreshAdditionalTextures() {
    _nodeTextures.bumpmap = nullptr;
    if (!_nodeTextures.diffuse) {
        return;
    }
    const Texture::Features &features = _nodeTextures.diffuse->features();
    if (!features.envmapTexture.empty()) {
        _nodeTextures.envmap = _resourceSvc.textures.get(features.envmapTexture, TextureUsage::EnvironmentMap).get();
    } else if (!features.bumpyShinyTexture.empty()) {
        _nodeTextures.envmap = _resourceSvc.textures.get(features.bumpyShinyTexture, TextureUsage::EnvironmentMap).get();
    }
    if (!features.bumpmapTexture.empty()) {
        _nodeTextures.bumpmap = _resourceSvc.textures.get(features.bumpmapTexture, TextureUsage::BumpMap).get();
    }
}

void MeshSceneNode::update(float dt) {
    SceneNode::update(dt);

    std::shared_ptr<ModelNode::TriangleMesh> mesh(_modelNode.mesh());
    if (mesh) {
        const auto oldUv = materialUv();
        const auto oldBumpFrame = _bumpmapCycleFrame;
        updateUVAnimation(dt, *mesh);
        updateCycleAnimation(dt);
        if (mesh->danglymesh) {
            updateDanglyAnimation(dt, *mesh->danglymesh);
        }
        if (mesh->saber) {
            updateSaberAnimation(dt);
        }
        if (oldBumpFrame != _bumpmapCycleFrame)
            _sceneGraph.gpuScene().patchBumpMapFrame(id(), _bumpmapCycleFrame);
        const auto newUv = materialUv();
        if (std::memcmp(&oldUv, &newUv, sizeof(newUv)) != 0) {
            _sceneGraph.gpuScene().patchMaterialUv(id(), newUv);
        }
    }
}

glm::mat3x4 MeshSceneNode::materialUv() const {
    // The scroll and the cycle compose into the one transform the shaders
    // already apply. A cycle is a window onto a sheet: scale the basis down to
    // one cell and translate onto the cell the clock has reached.
    glm::vec2 scale {1.0f};
    glm::vec2 offset {_uvOffset};
    if (const auto *diffuse = _nodeTextures.diffuse) {
        const auto &features = diffuse->features();
        if (features.procedureType == Texture::ProcedureType::Cycle &&
            (features.numX > 1 || features.numY > 1)) {
            const int numX = std::max(1, features.numX);
            const int numY = std::max(1, features.numY);
            const int col = _diffuseCycleFrame % numX;
            const int row = (_diffuseCycleFrame / numX) % numY;
            // Half a texel in from each edge of the cell, and the span one
            // texel shorter to match.
            //
            // Without it the window runs to the exact cell boundary, where the
            // bilinear tap straddles it and mixes the neighbouring frame in. On
            // a 128-texel cell filling most of the screen that is not a hairline
            // - it is a visible band of the wrong frame down the seam, which
            // reads as the animation landing between two frames. This is the
            // same half-texel clamp the text shader already applies to a glyph
            // for the same reason.
            const glm::vec2 sheet {std::max(1, diffuse->width()),
                                   std::max(1, diffuse->height())};
            const glm::vec2 cell {sheet.x / numX, sheet.y / numY};
            scale = (cell - 1.0f) / sheet;
            // The scroll is authored in whole-texture units. Inside a cycle
            // window it has to move by the window, or a texture that both
            // scrolls and cycles drags its window across the cell boundary and
            // shows two frames at once.
            offset = _uvOffset * scale + (glm::vec2 {col, row} * cell + 0.5f) / sheet;
        }
    }
    return glm::mat3x4(glm::vec4(scale.x, 0.0f, 0.0f, 0.0f),
                       glm::vec4(0.0f, scale.y, 0.0f, 0.0f),
                       glm::vec4(offset.x, offset.y, 0.0f, 0.0f));
}

/**
 * The frame a cycled texture is showing, from the game clock.
 *
 * floor of the wrapped time rather than round of the clamped time: rounding
 * gave the first and last frames half the duration of every other one, and the
 * reset depended on a float equality that only held because of the clamp.
 * KotOR.js computes exactly this expression in its vertex shader.
 */
int MeshSceneNode::cycleFrame(const graphics::Texture &texture, float time) {
    const auto &features = texture.features();
    const int frameCount = std::max(1, features.numX * features.numY);
    const float fps = features.fps > 0 ? static_cast<float>(features.fps) : 1.0f;
    return static_cast<int>(glm::floor(time * fps)) % frameCount;
}

void MeshSceneNode::updateUVAnimation(float dt, const ModelNode::TriangleMesh &mesh) {
    if (mesh.uvAnimation.dir.x != 0.0f || mesh.uvAnimation.dir.y != 0.0f) {
        _uvOffset += kUvAnimationSpeed * mesh.uvAnimation.dir * dt;
        _uvOffset -= glm::floor(_uvOffset);
    }
}

void MeshSceneNode::updateCycleAnimation(float dt) {
    // Every slot that can carry a cycle, not only the bumpmap. A TXI naming
    // proceduretype cycle with numX/numY above one is an animation whatever
    // slot it lands in - force fields, energy shields and scrolling panels are
    // DIFFUSE cycles - and only the bumpmap was ever stepped.
    _cycleTime += dt;
    if (const auto *diffuse = _nodeTextures.diffuse) {
        if (diffuse->features().procedureType == Texture::ProcedureType::Cycle) {
            _diffuseCycleFrame = cycleFrame(*diffuse, _cycleTime);
        }
    }
    if (const auto *bumpmap = _nodeTextures.bumpmap) {
        if (bumpmap->features().procedureType == Texture::ProcedureType::Cycle) {
            _bumpmapCycleFrame = cycleFrame(*bumpmap, _cycleTime);
        }
    }
}

static std::string vectorToString(const glm::vec3 &vec) {
    return str(boost::format("[%.04f, %.04f, %.04f]") % vec.x % vec.y % vec.z);
}

void MeshSceneNode::updateDanglyAnimation(float dt, const ModelNode::Danglymesh &mesh) {
    if (dt < 0.0125f) {
        dt = 0.0125f;
    } else if (dt > 0.035f) {
        dt = 0.035f;
    }
    glm::vec3 worldPos = _absTransform[3];
    glm::vec3 deltaPos {worldPos - _dangly.prevWorldPos};
    glm::vec3 objSpaceDeltaPos = _absTransformInv * glm::vec4 {worldPos - _dangly.prevWorldPos, 0.0f};

    _windTime = glm::mod(_windTime + dt, glm::two_pi<float>());
    auto wind = 0.01f * glm::vec3 {glm::abs(glm::sin(_windTime)), 0.0f, 0.0f};
    glm::vec3 objSpaceWind = _absTransformInv * glm::vec4 {wind, 0.0f};

    float deltaPosMag = glm::length(deltaPos);
    if (deltaPosMag <= 5.0f) {
        for (size_t i = 0; i < _dangly.vertices.size(); ++i) {
            if (mesh.constraints[i] == 0.0f) {
                continue;
            }
            auto &vertex = _dangly.vertices[i];
            auto displacement = vertex.displacement - objSpaceDeltaPos;

            glm::vec3 acceleration {0.0f};
            acceleration += -displacement * (0.5f * mesh.tightness * mesh.constraints[i]); // spring force
            acceleration += -vertex.velocity * (1.5f * mesh.period);                       // damp force
            vertex.velocity += acceleration * dt;
            displacement += vertex.velocity * dt;

            auto windScaled = 20.0f * mesh.displacement * (1.0f - mesh.constraints[i] / 255.0f) * objSpaceWind;
            displacement += windScaled;

            float dispmag = glm::length(displacement);
            if (dispmag > 0.0f) {
                float maxdisp = mesh.displacement * (1.0f - mesh.constraints[i] / 255.0f);
                vertex.displacement = glm::min(dispmag, maxdisp) * displacement / dispmag;
            } else {
                vertex.displacement = glm::vec3 {0.0f};
            }
        }
    }
    _dangly.prevWorldPos = std::move(worldPos);
}

void MeshSceneNode::updateSaberAnimation(float dt) {
    glm::vec3 worldPos = _absTransform[3];
    glm::vec3 deltaPos = worldPos - _saber.prevWorldPos;
    float deltaPosMag = glm::length(deltaPos);
    if (deltaPosMag > 1.0f) {
        _saber.displacement = glm::vec3 {0.0f};
    } else if (deltaPosMag > 0.0f) {
        // A retracted blade is scaled to nothing, and a singular transform has
        // no inverse: glm::inverse hands back infinities, the change of basis
        // below returns NaN, and NaN survives both the accumulation and the
        // decay for the rest of the object's life. Every lightsaber in the game
        // reaches this, because every lightsaber is created switched off - and
        // by the time it ignites the displacement is already poisoned, so the
        // blade never widens again however hard it is swung. The offset is
        // added to the source vertex before the object transform, so it is not
        // only the blur planes that come out NaN; it is the whole blade mesh.
        //
        // Zeroed rather than skipped, so the state that leaves here is one the
        // shader can use: a blade with no history to widen from is a blade at
        // rest, which is what a blade that does not exist yet should look like.
        glm::vec3 deltaLocal = _absTransformInv * glm::vec4 {deltaPos, 0.0f};
        if (glm::all(glm::isfinite(deltaLocal))) {
            _saber.displacement += deltaLocal;
        } else {
            _saber.displacement = glm::vec3 {0.0f};
        }
    }
    _saber.displacement -= _saber.displacement * glm::min(8.0f * dt, 1.0f);
    _saber.prevWorldPos = worldPos;
}

bool MeshSceneNode::shouldRender() const {
    auto mesh = _modelNode.mesh();
    if (!mesh || !mesh->render || _alpha == 0.0f) {
        return false;
    }
    // Renderability depends on the diffuse texture effectively bound to this node,
    // not on whether the model itself authored one. Creature bodies routinely defer
    // their skin to a runtime override applied through setMainTexture.
    return !_modelNode.isAABBMesh() && _nodeTextures.diffuse != nullptr;
}

bool MeshSceneNode::isTransparent() const {
    if (!_nodeTextures.diffuse) {
        return false;
    }
    auto blending = _nodeTextures.diffuse->features().blending;
    switch (blending) {
    case Texture::Blending::Additive:
        return true;
    case Texture::Blending::PunchThrough:
        return false;
    default:
        break;
    }
    if (_alpha < 1.0f) {
        return true;
    }
    if (_nodeTextures.envmap || _nodeTextures.bumpmap) {
        return false;
    }
    // A fully self-illuminated surface is opaque - a sky shell, a lit panel -
    // unless its texture authors an alpha test, which is the artist saying the
    // texel coverage is real. Without that exception a leaf card whose
    // selfIllum happens to be white renders as a solid quad.
    if ((1.0f - rgbToLuma(_selfIllumColor)) < 0.01f &&
        _nodeTextures.diffuse->features().alphaTest < 0.0f) {
        return false;
    }
    return hasAlphaChannel(_nodeTextures.diffuse->pixelFormat());
}

static bool isLightingEnabledByUsage(ModelUsage usage) {
    return usage != ModelUsage::Projectile;
}

// Reception used to be limited to rooms, so every creature, door, placeable
// and piece of equipment was lit as though the sun reached through whatever
// was standing in front of it - including the shadow the object cast itself.
// Anything occupying the world receives. GUI and camera models are not in the
// world at all, and background scenery is excluded by the caller, which knows
// whether this particular mesh resolved as sky.
/**
 * Everything lit receives shadows; only the authored backdrop is exempt,
 * because it is not lit at all.
 *
 * The usage whitelist this replaces predated the caster policy. It excluded
 * whole classes - grass, the sky shell's neighbours, anything not one of six
 * usages - so a shadow could fall on one surface and stop dead at the edge of
 * another for reasons the picture never explains. What a surface receives is
 * a property of the light reaching it, not of what kind of object it is.
 */
static bool isReceivingShadows(const ModelSceneNode &model, const MeshSceneNode &modelNode) {
    return true;
}

void MeshSceneNode::collectInto(GpuScene &scene) {
    auto mesh = _modelNode.mesh();
    // shouldRender only asks whether the model node names a diffuse map, not
    // whether that texture resolved. A missing resource leaves the pointer
    // null while the predicate stays true, and the material below dereferences
    // it, so the texture itself is part of being renderable.
    bool render = shouldRender() && _nodeTextures.diffuse;
    if (!mesh || !render) {
        scene.unregisterObject(id());
        return;
    }
    // Authored data contains mesh nodes with no vertices at all (tat_m18aa
    // and four other modules). There is nothing to draw or trace; letting one
    // through kills the scene upload, which cannot buffer zero bytes.
    if (mesh->mesh->vertexData().empty()) {
        scene.unregisterObject(id());
        return;
    }
    Material material;
    bool transparent = render && isTransparent();
    material.type = transparent
                        ? MaterialType::TransparentModel
                        : MaterialType::OpaqueModel;
    if (render) {
        material.textures[static_cast<size_t>(MaterialTextureSlot::MainTex)] = _nodeTextures.diffuse;
        if (_nodeTextures.lightmap) {
            material.textures[static_cast<size_t>(MaterialTextureSlot::Lightmap)] = _nodeTextures.lightmap;
        }
        if (_nodeTextures.envmap) {
            if (_nodeTextures.envmap->isCubeMap()) {
                material.textures[static_cast<size_t>(MaterialTextureSlot::EnvMapCube)] = _nodeTextures.envmap;
            } else {
                material.textures[static_cast<size_t>(MaterialTextureSlot::EnvMap)] = _nodeTextures.envmap;
            }
        }
        if (_nodeTextures.bumpmap) {
            if (_nodeTextures.bumpmap->isGrayscale()) {
                material.textures[static_cast<size_t>(MaterialTextureSlot::BumpMapArray)] = _nodeTextures.bumpmap;
                material.bumpMapFrame = _bumpmapCycleFrame;
            } else {
                material.textures[static_cast<size_t>(MaterialTextureSlot::NormalMap)] = _nodeTextures.bumpmap;
            }
        }
    }
    material.uv = materialUv();
    material.color = glm::vec4(1.0f, 1.0f, 1.0f, _alpha);
    material.ambientColor = mesh->ambient;
    material.diffuseColor = mesh->diffuse;
    material.selfIllumColor = _selfIllumColor;
    material.staticObject = _static;
    // Semantic sky, two authored sources: the per-mesh MDL background flag
    // (TSL and some models), or membership in a background-scenery room -
    // one without a walkmesh, the K1 skybox convention - refined by the
    // absence of a lightmap so decorative-but-lit geometry keeps its
    // authored lighting. Never color-based.
    material.backgroundGeometry = mesh->backgroundGeometry ||
                                  (_model.isBackgroundScenery() && !_nodeTextures.lightmap);
    // The manually curated per-name record - the mechanical level-by-level
    // pass that heuristics cannot replace. Class and material ops both.
    material.curatedIndex = _sceneGraph.gpuScene().traceMaterials().curatedIndex(
        _model.model().name(), _modelNode.name());
    if (render && _sceneGraph.hasShadowLight() && !material.backgroundGeometry &&
        isReceivingShadows(_model, *this)) {
        material.affectedByShadows = true;
    }
    if (render && _sceneGraph.isFogEnabled() && _model.model().isAffectedByFog()) {
        material.affectedByFog = true;
    }
    if (render) {
        material.faceCulling = _nodeTextures.diffuse->features().decal ? FaceCullMode::None : FaceCullMode::Back;
    }
    auto categories = renderCategory(transparent ? RenderCategory::Transparent : RenderCategory::Opaque);
    scene.addMesh(categories,
                  id(),
                  nameIds(),
                  *mesh->mesh, material, _absTransform, _absTransformInv,
                  _prevAbsTransform, buildDeformation(), &_model);
}

void MeshSceneNode::onGpuActivationChanged(bool active) {
    if (active)
        collectInto(_sceneGraph.gpuScene());
}

RegisteredDeformation MeshSceneNode::buildDeformation() {
    const auto mesh = _modelNode.mesh();
    if (!mesh)
        return {};
    if (_modelNode.isSkinMesh()) {
        R_PROFILE_ZONE("GpuScene::skin stream update");
        const auto &skin = *mesh->skin;
        _bones.assign(kMaxBones, glm::mat4(1.0f));
        for (size_t i = 0; i < kMaxBones; ++i) {
            if (i >= skin.boneNodeNumber.size()) {
                break;
            }
            auto nodeNumber = skin.boneNodeNumber[i];
            if (nodeNumber == 0xffff) {
                continue;
            }
            auto bone = _model.getNodeByNumber(nodeNumber);
            if (!bone) {
                continue;
            }
            _bones[i] = _modelNode.absoluteTransformInverse(); // convert bone transform in model space to bone transform in this model node space
            _bones[i] *= _model.absoluteTransformInverse();    // convert bone transform in world space to bone transform in model space
            _bones[i] *= bone->absoluteTransform();
            _bones[i] *= skin.boneMatrices[skin.boneSerial[i]]; // extract changes to the bone transform in this model node space
        }
        if (_prevBones.size() != _bones.size()) {
            _prevBones = _bones;
        }
        return RegisteredSkin {&_bones, &_prevBones};
    } else if (_modelNode.isDanglymesh()) {
        R_PROFILE_ZONE("GpuScene::dangly stream update");
        _danglyPositions.clear();
        _danglyPositions.reserve(_dangly.vertices.size());
        for (const auto &vertex : _dangly.vertices) {
            _danglyPositions.emplace_back(vertex.position + vertex.displacement, 1.0f);
        }
        if (_prevDanglyPositions.size() != _danglyPositions.size())
            _prevDanglyPositions = _danglyPositions;
        return RegisteredDangly {&_danglyPositions, &_prevDanglyPositions};
    } else if (_modelNode.isSaberMesh()) {
        R_PROFILE_ZONE("GpuScene::saber stream update");
        return RegisteredSaber {&_saber.displacement};
    }
    return {};
}

bool MeshSceneNode::hasDynamicDeformation() const {
    return _modelNode.isSkinMesh() || _modelNode.isDanglymesh() ||
           _modelNode.isSaberMesh();
}

void MeshSceneNode::refreshGpuSceneStreams(GpuScene &scene) {
    if (!scene.updateMeshDeformation(id(), buildDeformation()))
        collectInto(scene);
}

void MeshSceneNode::updateGpuStreams() {
    if (hasDynamicDeformation())
        (void)buildDeformation();
}

void MeshSceneNode::snapshotPreviousFrame(uint64_t frame) {
    if (_prevFrame == frame) {
        return;
    }
    _prevBones = _bones;
    _prevDanglyPositions.clear();
    _prevDanglyPositions.reserve(_dangly.vertices.size());
    for (const auto &vertex : _dangly.vertices) {
        _prevDanglyPositions.emplace_back(vertex.position + vertex.displacement, 1.0f);
    }
    SceneNode::snapshotPreviousFrame(frame);
    _sceneGraph.gpuScene().settleMeshTransform(id(), _absTransform);
}

bool MeshSceneNode::requiresPerFrameGpuSync() const {
    const auto mesh = _modelNode.mesh();
    if (!mesh)
        return false;
    return _modelNode.isSkinMesh() || _modelNode.isDanglymesh() ||
           _modelNode.isSaberMesh();
}

void MeshSceneNode::onAbsoluteTransformChanged() {
    _sceneGraph.gpuScene().updateMeshTransform(
        id(), _absTransform, _absTransformInv, _prevAbsTransform);
}

bool MeshSceneNode::isLightingEnabled() const {
    if (!isLightingEnabledByUsage(_model.usage())) {
        return false;
    }
    // Lighting is disabled when diffuse texture is additive
    if (_nodeTextures.diffuse && _nodeTextures.diffuse->features().blending == Texture::Blending::Additive) {
        return false;
    }
    return true;
}

void MeshSceneNode::setMainTexture(Texture *texture) {
    ModelNodeSceneNode::setMainTexture(texture);
    _nodeTextures.diffuse = texture;
    refreshAdditionalTextures();
    collectInto(_sceneGraph.gpuScene());
}

void MeshSceneNode::setEnvironmentMap(Texture *texture) {
    ModelNodeSceneNode::setEnvironmentMap(texture);
    _nodeTextures.envmap = std::move(texture);
    collectInto(_sceneGraph.gpuScene());
}

void MeshSceneNode::setAlpha(float alpha) {
    if (_alpha == alpha)
        return;
    _alpha = alpha;
    collectInto(_sceneGraph.gpuScene());
}

void MeshSceneNode::setSelfIllumColor(glm::vec3 color) {
    if (_selfIllumColor == color)
        return;
    _selfIllumColor = std::move(color);
    collectInto(_sceneGraph.gpuScene());
}

void MeshSceneNode::initDanglyMesh() {
    auto mesh = _modelNode.mesh();
    if (!mesh || !mesh->danglymesh) {
        return;
    }
    _dangly.vertices.reserve(mesh->danglymesh->positions.size());
    for (const auto &position : mesh->danglymesh->positions) {
        DanglyVertex vertex;
        vertex.position = position;
        _dangly.vertices.push_back(std::move(vertex));
    }
}

} // namespace scene

} // namespace reone
