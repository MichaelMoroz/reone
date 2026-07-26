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

#include "reone/graphics/uniforms.h"

#include "reone/graphics/backend.h"

#include "reone/graphics/context.h"

// Compiles the generated std140 assertions. Included here rather than from the
// header so that the check runs exactly once, in the translation unit that owns
// the uploads.
#include "reone/graphics/uniformlayout.generated.h"

namespace reone {

namespace graphics {

void Uniforms::init() {
    if (isVulkanBackend()) {
        // These are OpenGL uniform buffers, never initialised on the Vulkan
        // path. Vulkan writes its uniforms into a per-frame arena instead
        // (VulkanUniformRing), so callers shared with the GL path can keep
        // calling these and have them do nothing.
        return;
    }
    if (_inited) {
        return;
    }

    static GlobalUniforms defaultGlobals;
    static LocalUniforms defaultLocals;
    static BoneUniforms defaultBones;
    static DanglyUniforms defaultDangly;
    static ParticleUniforms defaultParticles;
    static GrassUniforms defaultGrass;
    static WalkmeshUniforms defaultWalkmesh;
    static AABBUniforms defaultAABB;
    static TextUniforms defaultText;
    static ScreenEffectUniforms defaultScreenEffect;

    _ubGlobals = initBuffer(&defaultGlobals, sizeof(GlobalUniforms));
    _ubLocals = initBuffer(&defaultLocals, sizeof(LocalUniforms));
    _ubBones = initBuffer(&defaultBones, sizeof(BoneUniforms));
    _ubDangly = initBuffer(&defaultDangly, sizeof(DanglyUniforms));
    _ubParticles = initBuffer(&defaultParticles, sizeof(ParticleUniforms));
    _ubGrass = initBuffer(&defaultGrass, sizeof(GrassUniforms));
    _ubWalkmesh = initBuffer(&defaultWalkmesh, sizeof(WalkmeshUniforms));
    _ubAABB = initBuffer(&defaultAABB, sizeof(AABBUniforms));
    _ubText = initBuffer(&defaultText, sizeof(TextUniforms));
    _ubScreenEffect = initBuffer(&defaultScreenEffect, sizeof(ScreenEffectUniforms));

    _context.bindUniformBuffer(*_ubGlobals, UniformBlockBindingPoints::globals);
    _context.bindUniformBuffer(*_ubLocals, UniformBlockBindingPoints::locals);
    _context.bindUniformBuffer(*_ubBones, UniformBlockBindingPoints::bones);
    _context.bindUniformBuffer(*_ubDangly, UniformBlockBindingPoints::dangly);
    _context.bindUniformBuffer(*_ubParticles, UniformBlockBindingPoints::particles);
    _context.bindUniformBuffer(*_ubGrass, UniformBlockBindingPoints::grass);
    _context.bindUniformBuffer(*_ubWalkmesh, UniformBlockBindingPoints::walkmesh);
    _context.bindUniformBuffer(*_ubAABB, UniformBlockBindingPoints::aabb);
    _context.bindUniformBuffer(*_ubText, UniformBlockBindingPoints::text);
    _context.bindUniformBuffer(*_ubScreenEffect, UniformBlockBindingPoints::screenEffect);

    _inited = true;
}

void Uniforms::deinit() {
    if (!_inited) {
        return;
    }

    _ubGlobals.reset();
    _ubLocals.reset();
    _ubBones.reset();
    _ubDangly.reset();
    _ubParticles.reset();
    _ubGrass.reset();
    _ubWalkmesh.reset();
    _ubAABB.reset();
    _ubText.reset();
    _ubScreenEffect.reset();

    _inited = false;
}

void Uniforms::setGlobals(const std::function<void(GlobalUniforms &)> &block) {
    block(_globals);
    if (isVulkanBackend()) {
        // The mirror above is still wanted - the Vulkan pipeline reads it and
        // pushes it into that frame's arena - but these are OpenGL buffers that
        // were never initialised, so the upload is skipped.
        return;
    }
    _context.bindUniformBuffer(*_ubGlobals, UniformBlockBindingPoints::globals);
    _ubGlobals->setData(&_globals, sizeof(GlobalUniforms));
}

void Uniforms::setLocals(const std::function<void(LocalUniforms &)> &block) {
    block(_locals);
    if (isVulkanBackend()) {
        // The mirror above is still wanted - the Vulkan pipeline reads it and
        // pushes it into that frame's arena - but these are OpenGL buffers that
        // were never initialised, so the upload is skipped.
        return;
    }
    _context.bindUniformBuffer(*_ubLocals, UniformBlockBindingPoints::locals);
    _ubLocals->setData(&_locals, sizeof(LocalUniforms));
}

void Uniforms::setBones(const std::function<void(BoneUniforms &)> &block) {
    block(_bones);
    if (isVulkanBackend()) {
        // The mirror above is still wanted - the Vulkan pipeline reads it and
        // pushes it into that frame's arena - but these are OpenGL buffers that
        // were never initialised, so the upload is skipped.
        return;
    }
    _context.bindUniformBuffer(*_ubBones, UniformBlockBindingPoints::bones);
    _ubBones->setData(&_bones, sizeof(BoneUniforms));
}

void Uniforms::setDangly(const std::function<void(DanglyUniforms &)> &block) {
    block(_dangly);
    if (isVulkanBackend()) {
        // The mirror above is still wanted - the Vulkan pipeline reads it and
        // pushes it into that frame's arena - but these are OpenGL buffers that
        // were never initialised, so the upload is skipped.
        return;
    }
    _context.bindUniformBuffer(*_ubDangly, UniformBlockBindingPoints::dangly);
    _ubDangly->setData(&_dangly, sizeof(DanglyUniforms));
}

void Uniforms::setParticles(const std::function<void(ParticleUniforms &)> &block) {
    block(_particles);
    if (isVulkanBackend()) {
        // The mirror above is still wanted - the Vulkan pipeline reads it and
        // pushes it into that frame's arena - but these are OpenGL buffers that
        // were never initialised, so the upload is skipped.
        return;
    }
    _context.bindUniformBuffer(*_ubParticles, UniformBlockBindingPoints::particles);
    _ubParticles->setData(&_particles, sizeof(ParticleUniforms));
}

void Uniforms::setGrass(const std::function<void(GrassUniforms &)> &block) {
    block(_grass);
    if (isVulkanBackend()) {
        // The mirror above is still wanted - the Vulkan pipeline reads it and
        // pushes it into that frame's arena - but these are OpenGL buffers that
        // were never initialised, so the upload is skipped.
        return;
    }
    _context.bindUniformBuffer(*_ubGrass, UniformBlockBindingPoints::grass);
    _ubGrass->setData(&_grass, sizeof(GrassUniforms));
}

void Uniforms::setWalkmesh(const std::function<void(WalkmeshUniforms &)> &block) {
    block(_walkmesh);
    if (isVulkanBackend()) {
        // The mirror above is still wanted - the Vulkan pipeline reads it and
        // pushes it into that frame's arena - but these are OpenGL buffers that
        // were never initialised, so the upload is skipped.
        return;
    }
    _context.bindUniformBuffer(*_ubWalkmesh, UniformBlockBindingPoints::walkmesh);
    _ubWalkmesh->setData(&_walkmesh, sizeof(WalkmeshUniforms));
}

void Uniforms::setAABB(const std::function<void(AABBUniforms &)> &block) {
    block(_aabb);
    if (isVulkanBackend()) {
        // The mirror above is still wanted - the Vulkan pipeline reads it and
        // pushes it into that frame's arena - but these are OpenGL buffers that
        // were never initialised, so the upload is skipped.
        return;
    }
    _context.bindUniformBuffer(*_ubAABB, UniformBlockBindingPoints::aabb);
    _ubAABB->setData(&_aabb, sizeof(AABBUniforms));
}

void Uniforms::setText(const std::function<void(TextUniforms &)> &block) {
    block(_text);
    if (isVulkanBackend()) {
        // The mirror above is still wanted - the Vulkan pipeline reads it and
        // pushes it into that frame's arena - but these are OpenGL buffers that
        // were never initialised, so the upload is skipped.
        return;
    }
    _context.bindUniformBuffer(*_ubText, UniformBlockBindingPoints::text);
    _ubText->setData(&_text, sizeof(TextUniforms));
}

void Uniforms::setScreenEffect(const std::function<void(ScreenEffectUniforms &)> &block) {
    block(_screenEffect);
    if (isVulkanBackend()) {
        // The mirror above is still wanted - the Vulkan pipeline reads it and
        // pushes it into that frame's arena - but these are OpenGL buffers that
        // were never initialised, so the upload is skipped.
        return;
    }
    _context.bindUniformBuffer(*_ubScreenEffect, UniformBlockBindingPoints::screenEffect);
    _ubScreenEffect->setData(&_screenEffect, sizeof(ScreenEffectUniforms));
}

std::unique_ptr<UniformBuffer> Uniforms::initBuffer(const void *data, ptrdiff_t size) {
    auto buf = std::make_unique<UniformBuffer>();
    buf->setData(data, size, false);
    buf->init();
    return buf;
}

} // namespace graphics

} // namespace reone
