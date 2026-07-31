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

#include "reone/graphics/uniformlayout.generated.h"

namespace reone {

namespace graphics {

void Uniforms::init() {
}

void Uniforms::setGlobals(const std::function<void(GlobalUniforms &)> &block) {
    block(_globals);
}

void Uniforms::setLocals(const std::function<void(LocalUniforms &)> &block) {
    block(_locals);
}

void Uniforms::setBones(const std::function<void(BoneUniforms &)> &block) {
    block(_bones);
}

void Uniforms::setDangly(const std::function<void(DanglyUniforms &)> &block) {
    block(_dangly);
}

void Uniforms::setParticles(const std::function<void(ParticleUniforms &)> &block) {
    block(_particles);
}

void Uniforms::setGrass(const std::function<void(GrassUniforms &)> &block) {
    block(_grass);
}

void Uniforms::setWalkmesh(const std::function<void(WalkmeshUniforms &)> &block) {
    block(_walkmesh);
}

void Uniforms::setAABB(const std::function<void(AABBUniforms &)> &block) {
    block(_aabb);
}

void Uniforms::setText(const std::function<void(TextUniforms &)> &block) {
    block(_text);
}

void Uniforms::setScreenEffect(const std::function<void(ScreenEffectUniforms &)> &block) {
    block(_screenEffect);
}

} // namespace graphics

} // namespace reone
