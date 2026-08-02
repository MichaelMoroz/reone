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

#include "reone/scene/di/module.h"

#include "reone/scene/render/pipeline/vulkan.h"

#include "reone/graphics/vulkan/renderer.h"
#include "reone/system/logutil.h"

#include "reone/game/types.h"

using namespace reone::game;

namespace reone {

namespace scene {

namespace {

/**
 * Locate the asset override tree. The engine runs from build/bin while the
 * configs are committed at the repository root, so walk upwards rather than
 * copying them into the build directory - a copy would mean the editor writes
 * curated material overrides somewhere a rebuild can clobber. A shipped build
 * puts `override` beside the executable and matches on the first step.
 */
std::filesystem::path findOverrideRoot(resource::GameID gameId) {
    const char *game = gameId == resource::GameID::TSL ? "k2" : "k1";
    auto dir = std::filesystem::current_path();
    for (int depth = 0; depth < 5; ++depth) {
        auto candidate = dir / "override";
        if (std::filesystem::is_directory(candidate))
            return candidate / game;
        if (!dir.has_parent_path() || dir.parent_path() == dir)
            break;
        dir = dir.parent_path();
    }
    return std::filesystem::current_path() / "override" / game;
}

} // namespace

void SceneModule::init() {
    _renderPipelineFactory = std::make_unique<RenderPipelineFactory>(
        _graphicsOpt,
        _graphics.meshRegistry(),
        _graphics.textureRegistry(),
        _graphics.uniforms());
    _graphs = std::make_unique<SceneGraphs>(
        *_renderPipelineFactory,
        _graphicsOpt,
        _graphics.services(),
        _audio.services(),
        _resource.services(),
        findOverrideRoot(_resource.gameId()));

    _services = std::make_unique<SceneServices>(*_graphs, *_renderPipelineFactory);

    // Init scenes
    _graphs->reserve(kSceneMain);
    _graphs->reserve(kSceneMainMenu);
    _graphs->reserve(kSceneCharGen);
    for (int i = 0; i < kNumClasses; ++i) {
        _graphs->reserve(str(boost::format("%s.%d") % kSceneClassSelect % i));
    }
    _graphs->reserve(kScenePortraitSelect);
    _graphs->reserve(kSceneCharacter);
}

std::unique_ptr<IRenderPipeline> RenderPipelineFactory::create(RenderMode mode,
                                                               glm::ivec2 targetSize,
                                                               GpuScene &scene) {
    if (!_vulkanRenderer) {
        throw std::logic_error("Vulkan renderer was not supplied to the pipeline factory");
    }
    if (mode == RenderMode::PathTracing && !_vulkanRenderer->device().rayQueryAvailable()) {
        warn("Path tracing needs ray-query acceleration structures and position fetch, "
             "which this device does not provide; rendering PBR instead.",
             LogChannel::Graphics);
        mode = RenderMode::PBR;
    }
    return std::make_unique<VulkanRenderPipeline>(
        std::move(targetSize), _options, *_vulkanRenderer, _uniforms, _meshRegistry, _textureRegistry,
        scene, mode == RenderMode::PathTracing);
}

void SceneModule::deinit() {
    _services.reset();

    _graphs.reset();
    _renderPipelineFactory.reset();
}

} // namespace scene

} // namespace reone
