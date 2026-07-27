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

#include "reone/scene/render/pass/pbr.h"

#include "reone/graphics/context.h"
#include "reone/graphics/material.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/pbrtextures.h"
#include "reone/graphics/shaderregistry.h"
#include "reone/graphics/texture.h"
#include "reone/graphics/uniforms.h"
#include "reone/system/logutil.h"

using namespace reone::graphics;

namespace reone {

namespace scene {

void PBRRenderPass::executeDraw(Mesh &mesh,
                         Material &material,
                         const glm::mat4 &transform,
                         const glm::mat4 &transformInv,
                         const glm::mat4 &prevTransform) {
    _geometryPath = GeometryPath::Static;
    withMaterialAppliedToContext(material, [&](auto &program) {
        _uniforms.setLocals([this, &material, &transform, &transformInv, &prevTransform](auto &locals) {
            locals.reset();
            locals.model = transform;
            locals.modelInv = transformInv;
            locals.prevModel = prevTransform;
            applyMaterialToLocals(material, locals);
        });
        mesh.draw(_statistic);
    });
}

void PBRRenderPass::withMaterialAppliedToContext(const Material &material, std::function<void(ShaderProgram &)> block) {
    static const std::unordered_map<MaterialType, std::string> kMatTypeToProgramId {
        {MaterialType::DirLightShadow, ShaderProgramId::dirLightShadows},     //
        {MaterialType::PointLightShadow, ShaderProgramId::pointLightShadows}, //
        {MaterialType::OpaqueModel, ShaderProgramId::pbrModelStatic},         //
        {MaterialType::TransparentModel, ShaderProgramId::oitModel},          //
        {MaterialType::Walkmesh, ShaderProgramId::pbrWalkmesh}                //
    };
    if (kMatTypeToProgramId.count(material.type) == 0) {
        throw std::invalid_argument(str(boost::format("Material type %1% is not associated with a shader program") % static_cast<int>(material.type)));
    }
    auto programId = kMatTypeToProgramId.at(material.type);
    if (material.type == MaterialType::OpaqueModel) {
        switch (_geometryPath) {
        case GeometryPath::Skinned:
            programId = ShaderProgramId::pbrModelSkinned;
            break;
        case GeometryPath::Dangly:
            programId = ShaderProgramId::pbrModelDangly;
            break;
        case GeometryPath::Saber:
            programId = ShaderProgramId::pbrModelSaber;
            break;
        default:
            break;
        }
    }
    auto &program = _shaderRegistry.get(programId);
    _context.useProgram(program);
    for (const auto &[unit, texture] : material.textures) {
        _context.bindTexture(texture, unit);
    }
    _envMapDerivedLayer = 0;
    auto envMapIt = material.textures.find(TextureUnits::envMapCube);
    if (envMapIt == material.textures.end()) {
        envMapIt = material.textures.find(TextureUnits::envMap);
    }
    if (envMapIt != material.textures.end()) {
        auto &envMap = envMapIt->second.get();
        if (_options.pbr) {
            auto layer = _pbrTextures.findEnvMapDerivedLayer(envMap.name());
            if (layer) {
                _envMapDerivedLayer = *layer;
            } else {
                _envMapDerivedLayer = 0;
                _pbrTextures.requestEnvMapDerived({envMap});
            }
        }
    }
    auto prevBlending = _context.blendMode();
    if (material.blending && *material.blending != prevBlending) {
        _context.pushBlendMode(*material.blending);
    }
    auto prevFaceCulling = _context.faceCullMode();
    if (material.faceCulling && *material.faceCulling != prevFaceCulling) {
        _context.pushFaceCullMode(*material.faceCulling);
    }
    auto prevPolygonMode = _context.polygonMode();
    if (material.polygonMode && *material.polygonMode != prevPolygonMode) {
        _context.pushPolygonMode(*material.polygonMode);
    }
    block(program);
    if (material.blending && *material.blending != prevBlending) {
        _context.popBlendMode();
    }
    if (material.faceCulling && *material.faceCulling != prevFaceCulling) {
        _context.popFaceCullMode();
    }
    if (material.polygonMode && *material.polygonMode != prevPolygonMode) {
        _context.popPolygonMode();
    }
}

void PBRRenderPass::executeDrawSkinned(Mesh &mesh,
                                Material &material,
                                const glm::mat4 &transform,
                                const glm::mat4 &transformInv,
                                const glm::mat4 &prevTransform,
                                const std::vector<glm::mat4> &bones,
                                const std::vector<glm::mat4> &prevBones) {
    _geometryPath = GeometryPath::Skinned;
    withMaterialAppliedToContext(material, [&](auto &program) {
        _uniforms.setLocals([this, &material, &transform, &transformInv, &prevTransform](auto &locals) {
            locals.reset();
            locals.featureMask |= UniformsFeatureFlags::skin;
            locals.model = transform;
            locals.modelInv = transformInv;
            locals.prevModel = prevTransform;
            applyMaterialToLocals(material, locals);
        });
        _uniforms.setBones([&bones, &prevBones](auto &b) {
            std::memcpy(b.bones, &bones[0], kMaxBones * sizeof(glm::mat4));
            std::memcpy(b.prevBones, &prevBones[0], kMaxBones * sizeof(glm::mat4));
        });
        mesh.draw(_statistic);
    });
}

void PBRRenderPass::executeDrawDangly(Mesh &mesh,
                               Material &material,
                               const glm::mat4 &transform,
                               const glm::mat4 &transformInv,
                               const glm::mat4 &prevTransform,
                               const std::vector<glm::vec4> &positions) {
    _geometryPath = GeometryPath::Dangly;
    withMaterialAppliedToContext(material, [&](auto &program) {
        _uniforms.setLocals([this, &material, &transform, &transformInv, &prevTransform](auto &locals) {
            locals.reset();
            locals.featureMask |= UniformsFeatureFlags::dangly;
            locals.model = transform;
            locals.modelInv = transformInv;
            locals.prevModel = prevTransform;
            applyMaterialToLocals(material, locals);
        });
        _uniforms.setDangly([&positions](auto &dangly) {
            auto numPositions = std::min<int>(kMaxDanglyVertices, positions.size());
            std::memcpy(dangly.positions, &positions[0], numPositions * sizeof(glm::vec4));
        });
        mesh.draw(_statistic);
    });
}

void PBRRenderPass::executeDrawSaber(Mesh &mesh,
                              Material &material,
                              const glm::mat4 &transform,
                              const glm::mat4 &transformInv,
                              const glm::mat4 &prevTransform,
                              const glm::vec4 &displacement) {
    _geometryPath = GeometryPath::Saber;
    withMaterialAppliedToContext(material, [&](auto &program) {
        _uniforms.setLocals([this, &material, &transform, &transformInv, &prevTransform, &displacement](auto &locals) {
            locals.reset();
            locals.featureMask |= UniformsFeatureFlags::saber;
            locals.model = transform;
            locals.modelInv = transformInv;
            locals.prevModel = prevTransform;
            locals.saberDisplacement = displacement;
            applyMaterialToLocals(material, locals);
        });
        mesh.draw(_statistic);
    });
}

void PBRRenderPass::executeDrawBillboard(Texture &texture,
                                  const glm::vec4 &color,
                                  const glm::mat4 &transform,
                                  const glm::mat4 &transformInv,
                                  std::optional<float> size) {
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::billboard));
    _context.bindTexture(texture, TextureUnits::mainTex);
    _uniforms.setLocals([&transform, &transformInv, &size, &color](auto &locals) {
        locals.reset();
        locals.model = transform;
        locals.modelInv = transformInv;
        locals.color = color;
        if (size) {
            locals.featureMask |= UniformsFeatureFlags::fixedsize;
            locals.billboardSize = *size;
        }
    });
    // Depth off as well as additive: a lens flare stands for a light the caller
    // has already traced to, so it is meant to be seen through whatever is in
    // front of it. This used to be an ambient context scope around the caller,
    // which made it a raw GL call on a path the Vulkan backend also takes.
    _context.pushBlendMode(BlendMode::Additive);
    _context.withDepthTestMode(DepthTestMode::None, [this]() {
        _meshRegistry.get(MeshName::billboard).draw(_statistic);
    });
    _context.popBlendMode();
}

void PBRRenderPass::executeDrawParticles(Material &material,
                                  const glm::ivec2 &gridSize,
                                  const std::vector<ParticleInstance> &particles) {
    auto &texture = material.textures.at(TextureUnits::mainTex).get();
    auto faceCulling = material.faceCulling.value_or(FaceCullMode::Back);
    bool premultipliedAlpha = material.blending == BlendMode::Lighten;
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::oitParticles));
    _context.bindTexture(texture, TextureUnits::mainTex);
    _uniforms.setLocals([&premultipliedAlpha](auto &locals) {
        locals.reset();
        if (premultipliedAlpha) {
            locals.featureMask |= UniformsFeatureFlags::premulalpha;
        }
    });
    _uniforms.setParticles([&gridSize, &premultipliedAlpha, &particles](auto &p) {
        p.gridSize = gridSize;
        for (size_t i = 0; i < particles.size(); ++i) {
            const auto &particle = particles[i];
            p.particles[i].positionFrame = glm::vec4(particle.position, static_cast<float>(particle.frame));
            p.particles[i].size = particle.size;
            p.particles[i].color = particle.color;
            p.particles[i].right = glm::vec4(particle.right, 0.0f);
            p.particles[i].up = glm::vec4(particle.up, 0.0f);
        }
    });
    auto prevFaceCulling = _context.faceCullMode();
    if (faceCulling != prevFaceCulling) {
        _context.pushFaceCullMode(faceCulling);
    }
    _meshRegistry.get(MeshName::billboard).drawInstanced(particles.size(), _statistic);
    if (faceCulling != prevFaceCulling) {
        _context.popFaceCullMode();
    }
}

void PBRRenderPass::executeDrawGrass(float radius,
                              float quadSize,
                              Material &material,
                              const std::vector<GrassInstance> &instances) {
    auto &texture = material.textures.at(TextureUnits::mainTex).get();
    auto lightmap = material.textures.find(TextureUnits::lightmap);
    bool hasLightmap = lightmap != material.textures.end();
    _context.useProgram(_shaderRegistry.get(ShaderProgramId::pbrGrass));
    _context.bindTexture(texture, TextureUnits::mainTex);
    if (hasLightmap) {
        _context.bindTexture(lightmap->second.get(), TextureUnits::lightmap);
    }
    _uniforms.setLocals([hasLightmap](auto &locals) {
        locals.reset();
        locals.featureMask |= UniformsFeatureFlags::hashedalphatest;
        if (hasLightmap) {
            locals.featureMask |= UniformsFeatureFlags::lightmap;
        }
    });
    _uniforms.setGrass([&radius, &quadSize, &instances](auto &grass) {
        grass.radius = radius;
        grass.quadSize = glm::vec2(quadSize);
        for (size_t i = 0; i < instances.size(); ++i) {
            const auto &instance = instances[i];
            grass.clusters[i].positionVariant = glm::vec4(instance.position, static_cast<float>(instance.variant));
            grass.clusters[i].lightmapUV = instance.lightmapUV;
        }
    });
    _meshRegistry.get(MeshName::grass).drawInstanced(instances.size(), _statistic);
}

void PBRRenderPass::applyMaterialToLocals(const Material &material,
                                          LocalUniforms &locals) {
    // Resolved by withMaterialAppliedToContext, which always runs first.
    locals.envMapDerivedLayer = _envMapDerivedLayer;
    locals.featureMask |= graphics::materialFeatureMask(material);
    locals.uv = material.uv;
    locals.color = material.color;
    locals.ambientColor = glm::vec4 {material.ambientColor, 0.0f};
    locals.diffuseColor = glm::vec4 {material.diffuseColor, 0.0f};
    locals.selfIllumColor = glm::vec4(material.selfIllumColor, 1.0f);
    if (material.textures.count(TextureUnits::mainTex) > 0) {
        const auto &mainTex = material.textures.at(TextureUnits::mainTex).get();
        if (mainTex.features().waterAlpha != -1.0f) {
            locals.waterAlpha = mainTex.features().waterAlpha;
        }
    }
    if (material.textures.count(TextureUnits::bumpMapArray) > 0) {
        const auto &bumpmap = material.textures.at(TextureUnits::bumpMapArray).get();
        locals.bumpMapFrame = material.bumpMapFrame;
        locals.bumpMapScale = bumpmap.features().bumpMapScaling;
    }
}

void PBRRenderPass::executeDrawAABB(const std::vector<glm::vec4> &corners) {
    auto &program = _shaderRegistry.get(ShaderProgramId::pbrAABB);
    _context.useProgram(program);
    _uniforms.setAABB([&corners](auto &aabb) {
        std::memcpy(aabb.corners, &corners[0], std::min<size_t>(8, corners.size()) * sizeof(glm::vec4));
    });
    _context.withDepthMask(false, [this]() {
        _context.withPolygonMode(PolygonMode::Line, [this]() {
            _meshRegistry.get(MeshName::aabb).draw(_statistic);
        });
    });
}


} // namespace scene

} // namespace reone
