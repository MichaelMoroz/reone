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

#include "editor.h"
#include "engine.h"
#include "reone/game/types.h"
#include "reone/graphics/backend.h"
#include "reone/graphics/context.h"
#include "reone/graphics/di/services.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/shaderprogram.h"
#include "reone/graphics/shaderregistry.h"
#include "reone/graphics/textureutil.h"
#include "reone/resource/resources.h"
#include "reone/scene/graph.h"
#include "reone/scene/graphs.h"
#include "reone/scene/render/pipeline.h"
#include "reone/system/stringutil.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder, for the default right-hand layout
#include "imgui_stdlib.h"

#include <algorithm>
#include <numeric>

namespace reone {

// Editor::handle should take priority over ImGui event processing, so it close
// ImGui when it is in focus.
bool Editor::handle(const input::Event &event) {
    if (event.type != input::EventType::KeyUp ||
        event.key.code != input::KeyCode::F1) {
        return false;
    }
    _enabled ^= 1;
    return true;
}

void TableFilter::setupRowIndexFilter() {
    ImGui::TableNextColumn();
    ImGui::PushItemWidth(-FLT_MIN);
    if (ImGui::InputText("##TableFilter::setupRowIndexFilter", &_rowIndexStr)) {
        _rowIndex = string_to<int>(_rowIndexStr);
    }
    ImGui::PopItemWidth();
    _nextRowIndex = 0;
}

void TableFilter::setupColumnFilters() {
    int index = 0;
    for (std::string &filter : _columnFilters) {
        ImGui::TableNextColumn();
        ImGui::PushID(index++);
        ImGui::PushItemWidth(-FLT_MIN);
        ImGui::InputText("##TableFilter::setupColumnFilters", &filter);
        ImGui::PopItemWidth();
        ImGui::PopID();
    }
    _nextColumnIndex = 0;
}

bool TableFilter::nextRow() {
    bool skip = _rowIndex && _rowIndex != _nextRowIndex;
    ++_nextRowIndex;
    _nextColumnIndex = 0;
    return skip;
}

bool TableFilter::nextColumn(std::string_view column) {
    assert(_nextColumnIndex < _columnFilters.size() && "unexpected number of columns, missing setNumColumns?");
    std::string_view filter = _columnFilters[_nextColumnIndex];
    bool skip = !filter.empty() && (column.find(filter) == std::string::npos);
    ++_nextColumnIndex;
    return skip;
}

void Editor::twoDaRes(const resource::ResourceId &res, TwoDaTableContext &context) {
    std::shared_ptr<resource::TwoDA> td = _engine._resourceModule->twoDas().get(res.resRef.value());
    if (!td) {
        return;
    }

    dockNext();
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(res.resRef.value().c_str(), &context.show, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Hideable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_SortTristate | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Reorderable | ImGuiTableFlags_HighlightHoveredColumn;

    ImVec2 outerSize(0.0f, 0.0f);
    if (ImGui::BeginTable("##2DA", td->columns().size() + 1, flags, outerSize)) {
        ImGui::TableSetupColumn("#");
        for (const std::string &column : td->columns()) {
            ImGui::TableSetupColumn(column.c_str());
        }
        ImGui::TableSetupScrollFreeze(1, 2);
        ImGui::TableHeadersRow();

        context.filter.setNumColumns(td->columns().size());
        context.filter.setupRowIndexFilter();
        context.filter.setupColumnFilters();
        ImGui::TableNextRow();

        size_t rowIndex = 0;
        for (const resource::TwoDA::Row &row : td->rows()) {

            bool shouldSkip = context.filter.nextRow();
            if (!shouldSkip) {
                for (const std::string &column : row.values) {
                    if (context.filter.nextColumn(column)) {
                        shouldSkip = true;
                        break;
                    }
                }
            }

            if (shouldSkip) {
                ++rowIndex;
                continue;
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", rowIndex++);
            for (const std::string &v : row.values) {
                ImGui::TableNextColumn();
                ImGui::Text("%s", v.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void Editor::twoDa() {
    dockNext();
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("2DA", &_showTwoDa, ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    static char filterBuf[32] = "";
    ImGui::InputText("Filter", filterBuf, IM_ARRAYSIZE(filterBuf));

    if (ImGui::BeginListBox("##2DA list", ImVec2(-FLT_MIN, 20 * ImGui::GetTextLineHeightWithSpacing()))) {
        // ResourceModule exposes IResources, which has no container enumeration -
        // that lives on the concrete Resources, which is what it always holds.
        auto &resources = static_cast<resource::Resources &>(_engine._resourceModule->resources());
        size_t i = 0;
        for (const auto &container : resources.containers()) {
            for (const resource::ResourceId &res : container.provider->resourceIds()) {
                ++i;
                if (res.type != resource::ResType::TwoDA) {
                    continue;
                }
                std::string_view filter(filterBuf);
                const std::string &resName = res.resRef.value();
                if (!filter.empty() && resName.find(filter) == std::string::npos) {
                    continue;
                }

                ImGui::PushID(i);
                if (ImGui::Selectable(resName.c_str(), /*selected=*/false, ImGuiSelectableFlags_AllowDoubleClick)) {
                    // TODO: allow multiple copies of the same 2da
                    _twoDaContext[res].show = true;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndListBox();
    }
    ImGui::End();
}

void Editor::imGuiDemo() {
    dockNext();
    ImGui::ShowDemoWindow(&_showImGuiDemo);
}

void Editor::graphicsSettings() {
    dockNext();
    ImGui::SetNextWindowSize(ImVec2(410, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics settings", &_showGraphicsSettings)) {
        ImGui::End();
        return;
    }
    auto &options = _engine._options.graphics;

    // Which backend is running is decided by a command line flag before any of
    // this exists, so it is shown rather than offered.
    ImGui::Text("Backend: %s", graphics::isVulkanBackend() ? "Vulkan" : "OpenGL");
    ImGui::Spacing();

    ImGui::TextUnformatted("Live (applies next frame)");
    ImGui::Separator();
    ImGui::Checkbox("FXAA", &options.fxaa);
    ImGui::Checkbox("Sharpen", &options.sharpen);
    ImGui::Checkbox("SSAO", &options.ssao);
    ImGui::Checkbox("SSR", &options.ssr);
    ImGui::Checkbox("Grass", &options.grass);
    ImGui::Checkbox("TAA jitter", &options.taaJitter);
    ImGui::SliderFloat("Draw distance", &options.drawDistance, 1.0f, 1000.0f, "%.0f");

    ImGui::Spacing();
    ImGui::TextUnformatted("Requires graphics rebuild");
    ImGui::Separator();
    if (_pendingWidth == 0) {
        _pendingWidth = options.width;
        _pendingHeight = options.height;
        _pendingShadowResolution = options.shadowResolution;
        _pendingVsync = options.vsync;
    }
    static const struct {
        const char *name;
        int width;
        int height;
    } kResolutions[] {
        {"1280x720", 1280, 720},
        {"1600x900", 1600, 900},
        {"1920x1080", 1920, 1080},
        {"2560x1440", 2560, 1440},
        {"3840x2160", 3840, 2160}};
    std::string current = std::to_string(_pendingWidth) + "x" + std::to_string(_pendingHeight);
    if (ImGui::BeginCombo("Preset", current.c_str())) {
        for (const auto &res : kResolutions) {
            bool selected = res.width == _pendingWidth && res.height == _pendingHeight;
            if (ImGui::Selectable(res.name, selected)) {
                _pendingWidth = res.width;
                _pendingHeight = res.height;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::InputInt("Width", &_pendingWidth);
    ImGui::InputInt("Height", &_pendingHeight);
    ImGui::SliderInt("Shadow resolution", &_pendingShadowResolution, 512, 8192, "%d", ImGuiSliderFlags_Logarithmic);
    ImGui::Checkbox("V-sync", &_pendingVsync);
    if (ImGui::Button("Rebuild graphics targets")) {
        options.width = std::max(1, _pendingWidth);
        options.height = std::max(1, _pendingHeight);
        options.shadowResolution = _pendingShadowResolution;
        options.vsync = _pendingVsync;
        _engine.requestGraphicsRebuild();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Requires scene or asset reload");
    ImGui::Separator();
    bool pbr = options.pbr;
    int textureQuality = static_cast<int>(options.textureQuality);
    int anisotropic = options.anisotropicFiltering;
    ImGui::BeginDisabled();
    ImGui::Checkbox("PBR", &pbr);
    ImGui::SliderInt("Texture quality", &textureQuality, 0, 2);
    ImGui::SliderInt("Anisotropic filtering", &anisotropic, 1, 16);
    ImGui::EndDisabled();
    ImGui::TextDisabled("These settings take effect after reloading.");
    ImGui::End();
}

void Editor::frameTimes() {
    dockNext();
    ImGui::SetNextWindowSize(ImVec2(480, 370), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Frame times", &_showFrameTimes)) {
        ImGui::End();
        return;
    }
    auto slots = _engine._profiler->frameTimes("main");
    if (std::any_of(slots.begin(), slots.end(), [](const auto &slot) { return slot.empty(); })) {
        ImGui::TextUnformatted("Collecting samples...");
        ImGui::End();
        return;
    }
    size_t count = slots[0].size();
    for (const auto &slot : slots) {
        count = std::min(count, slot.size());
    }
    std::vector<float> total(count, 0.0f);
    for (const auto &slot : slots) {
        for (size_t i = 0; i < total.size(); ++i) {
            total[i] += slot[i] * 1000.0f;
        }
    }
    float mean = std::accumulate(total.begin(), total.end(), 0.0f) / total.size();
    auto sorted = total;
    std::sort(sorted.begin(), sorted.end());
    float onePercentWorst = sorted[std::max<size_t>(0, sorted.size() * 99 / 100)];
    ImGui::Text("Mean %.2f ms (%.1f FPS)   1%% worst %.2f ms", mean, 1000.0f / mean, onePercentWorst);
    ImGui::PlotLines("Frame time (ms)", total.data(), static_cast<int>(total.size()), 0, nullptr, 0.0f,
                     std::max(33.0f, *std::max_element(total.begin(), total.end()) * 1.1f), ImVec2(-FLT_MIN, 130));
    static constexpr const char *names[] = {"Input", "Update", "Graphics render", "Audio render"};
    for (size_t i = 0; i < slots.size(); ++i) {
        slots[i].resize(count);
        for (float &sample : slots[i]) {
            sample *= 1000.0f;
        }
        ImGui::PlotLines(names[i], slots[i].data(), static_cast<int>(slots[i].size()), 0, nullptr, 0.0f,
                         std::max(16.0f, *std::max_element(slots[i].begin(), slots[i].end()) * 1.1f), ImVec2(-FLT_MIN, 42));
    }
    ImGui::End();
}

static constexpr int kPreviewWidth = 640;
static constexpr int kPreviewHeight = 360;

static const char *kDebugModeNames[] {
    "Color",
    "Depth (linearised)",
    "Eye normal",
    "Motion (biased)",
    "Motion (flow)"};

static int defaultModeFor(scene::RenderTargetKind kind) {
    switch (kind) {
    case scene::RenderTargetKind::Depth:
        return 1;
    case scene::RenderTargetKind::EyeNormal:
        return 2;
    case scene::RenderTargetKind::Motion:
        return 4;
    default:
        return 0;
    }
}

static float defaultScaleFor(scene::RenderTargetKind kind) {
    // Motion vectors are a fraction of a screen width per frame, so they need
    // a large multiplier before anything is visible.
    return kind == scene::RenderTargetKind::Motion ? 50.0f : 1.0f;
}

void Editor::renderTargets() {
    dockNext();
    ImGui::SetNextWindowSize(ImVec2(520, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Render targets", &_showRenderTargets)) {
        ImGui::End();
        return;
    }
    _rtSource = nullptr;

    auto &graphs = _engine._sceneModule->graphs();
    auto sceneNames = graphs.sceneNames();
    if (sceneNames.empty()) {
        ImGui::TextUnformatted("No scenes registered.");
        ImGui::End();
        return;
    }
    if (_rtScene.empty() || sceneNames.count(_rtScene) == 0) {
        // The first name alphabetically is a portrait scene, which is only
        // rendered while a portrait is on screen and so usually has no
        // pipeline at all. Landing there shows an empty panel and reads as the
        // viewer being broken, so prefer the scene the game is actually in.
        auto main = sceneNames.find(game::kSceneMain);
        _rtScene = main != sceneNames.end() ? *main : *sceneNames.begin();
    }
    if (ImGui::BeginCombo("Scene", _rtScene.c_str())) {
        for (const auto &name : sceneNames) {
            if (ImGui::Selectable(name.c_str(), name == _rtScene)) {
                _rtScene = name;
            }
        }
        ImGui::EndCombo();
    }

    auto *pipeline = graphs.get(_rtScene).renderPipeline();
    if (!pipeline) {
        ImGui::TextWrapped("This scene has not been rendered yet, so it has no pipeline.");
        ImGui::End();
        return;
    }
    auto targets = pipeline->targets();
    if (targets.empty()) {
        ImGui::TextWrapped("This pipeline exposes no targets.");
        ImGui::End();
        return;
    }

    auto selected = std::find_if(targets.begin(), targets.end(), [this](auto &t) { return t.name == _rtTarget; });
    if (selected == targets.end()) {
        selected = targets.begin();
        _rtTarget = selected->name;
        _rtAutoMode = true;
    }
    if (ImGui::BeginCombo("Target", _rtTarget.c_str())) {
        for (const auto &target : targets) {
            if (ImGui::Selectable(target.name.c_str(), target.name == _rtTarget)) {
                _rtTarget = target.name;
                _rtAutoMode = true;
            }
        }
        ImGui::EndCombo();
    }

    if (_rtAutoMode) {
        _rtMode = defaultModeFor(selected->kind);
        _rtScale = defaultScaleFor(selected->kind);
        _rtAutoMode = false;
    }
    if (ImGui::Combo("Mode", &_rtMode, kDebugModeNames, IM_ARRAYSIZE(kDebugModeNames))) {
        _rtAutoMode = false;
    }
    ImGui::SliderFloat("Scale", &_rtScale, 0.1f, 200.0f, "%.1f", ImGuiSliderFlags_Logarithmic);

    if (selected->kind == scene::RenderTargetKind::Motion) {
        ImGui::TextWrapped(
            "Panning should tint the whole frame uniformly, strafing should band "
            "it by depth, and a moving character should stand out against a still "
            "background.");
    }

    _rtSource = selected->texture;
    auto preview = pipeline->renderTargetPreview(selected->name, _rtMode, _rtScale);
    if (preview) {
        ImGui::Image(reinterpret_cast<ImTextureID>(preview),
                     ImVec2(kPreviewWidth, kPreviewHeight));
    } else if (_rtPreviewColor) {
        // Flipped vertically: OpenGL's origin is bottom-left, ImGui's is top-left.
        ImGui::Image(
            static_cast<ImTextureID>(_rtPreviewColor->nameGL()),
            ImVec2(kPreviewWidth, kPreviewHeight),
            ImVec2(0.0f, 1.0f),
            ImVec2(1.0f, 0.0f));
    }
    ImGui::End();
}

void Editor::render() {
    if (!_enabled || !_rtSource) {
        return;
    }
    auto &graphicsSvc = _engine._services->graphics;

    if (!_rtPreview) {
        _rtPreviewColor = std::make_shared<graphics::Texture>(
            "editor_rt_preview",
            graphics::TextureType::TwoDim,
            graphics::getTextureProperties(graphics::TextureUsage::ColorBuffer));
        _rtPreviewColor->clear(kPreviewWidth, kPreviewHeight, graphics::PixelFormat::RGBA8);
        _rtPreviewColor->init();

        _rtPreview = std::make_unique<graphics::Framebuffer>();
        _rtPreview->attachColorDepth(_rtPreviewColor, nullptr);
        _rtPreview->init();
    }

    auto &program = graphicsSvc.shaderRegistry.get(graphics::ShaderProgramId::postDebugTexture);
    graphicsSvc.context.useProgram(program);
    program.setUniform("uDebugMode", _rtMode);
    program.setUniform("uDebugScale", _rtScale);
    graphicsSvc.context.bindDrawFramebuffer(*_rtPreview, {0});
    graphicsSvc.context.bindTexture(*_rtSource);
    graphicsSvc.context.withViewport(glm::ivec4(0, 0, kPreviewWidth, kPreviewHeight), [&graphicsSvc]() {
        graphicsSvc.meshRegistry.get(graphics::MeshName::quadNDC).draw(graphicsSvc.statistic);
    });
    graphicsSvc.context.resetDrawFramebuffer();
}

void Editor::update(float dt) {
    if (!_enabled) {
        return;
    }

    // Submitted before the dockspace so the viewport work area excludes it.
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Tools")) {
            ImGui::MenuItem("2DA", nullptr, &_showTwoDa);
            ImGui::MenuItem("Render targets", nullptr, &_showRenderTargets);
            ImGui::MenuItem("Graphics settings", nullptr, &_showGraphicsSettings);
            ImGui::MenuItem("Frame times", nullptr, &_showFrameTimes);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug")) {
            auto &shaderRegistry = _engine._graphicsModule->shaderRegistry();
            size_t variants = shaderRegistry.slangVariantCount();
            bool useSlang = shaderRegistry.useSlangVariants();
            if (ImGui::MenuItem("Slang shaders", nullptr, &useSlang, variants > 0)) {
                // Every variant is built at startup; the registry hands out the
                // transpiled build from the next draw on.
                shaderRegistry.setUseSlangVariants(useSlang);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (variants > 0) {
                    ImGui::SetTooltip("%zu of the shader programs have a transpiled twin.", variants);
                } else {
                    ImGui::SetTooltip("This build has no transpiled shaders - slangc was not found.");
                }
            }
            ImGui::MenuItem("ImGui Demo", nullptr, &_showImGuiDemo);
            ImGui::EndMenu();
        }
        // After the menus rather than before them. SameLine positions the next
        // item relative to the previous one, and with nothing yet on the bar
        // there is no previous one, so the readout was placed nowhere and
        // never appeared.
        {
            auto slots = _engine._profiler->frameTimes("main");
            float latest = 0.0f;
            for (const auto &slot : slots) {
                if (!slot.empty()) {
                    latest += slot.back();
                }
            }
            ImGui::SameLine(ImGui::GetWindowWidth() - 200.0f);
            if (latest > 0.0f) {
                ImGui::Text("%.1f FPS  %.2f ms", 1.0f / latest, latest * 1000.0f);
            } else {
                ImGui::TextUnformatted("FPS collecting...");
            }
        }
        ImGui::EndMainMenuBar();
    }

    dockSpace();

    if (_showRenderTargets) {
        renderTargets();
    } else {
        _rtSource = nullptr;
    }

    if (_showGraphicsSettings) {
        graphicsSettings();
    }

    if (_showFrameTimes) {
        frameTimes();
    }

    if (_showTwoDa) {
        twoDa();
    }

    for (auto &[res, context] : _twoDaContext) {
        if (context.show) {
            twoDaRes(res, context);
        }
    }

    if (_showImGuiDemo) {
        imGuiDemo();
    }
}

void Editor::dockSpace() {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();

    // PassthruCentralNode leaves the middle of the screen transparent, so the
    // game keeps rendering behind the docked windows.
    ImGuiID rootId = ImGui::DockSpaceOverViewport(
        0,
        viewport,
        ImGuiDockNodeFlags_PassthruCentralNode);

    if (_dockLayoutBuilt) {
        return;
    }
    _dockLayoutBuilt = true;

    // A layout restored from imgui.ini is the user's, so adopt its right-hand
    // node instead of overwriting it. Only a fresh profile gets the default.
    ImGuiDockNode *existing = ImGui::DockBuilderGetNode(rootId);
    if (existing && existing->IsSplitNode() && existing->ChildNodes[1]) {
        _rightDockId = existing->ChildNodes[1]->ID;
        return;
    }

    // Split a quarter off the right edge and remember it, so that windows opened
    // later dock there by default and stack as tabs.
    ImGui::DockBuilderRemoveNode(rootId);
    ImGui::DockBuilderAddNode(rootId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(rootId, viewport->WorkSize);
    ImGui::DockBuilderSplitNode(rootId, ImGuiDir_Right, 0.25f, &_rightDockId, nullptr);
    ImGui::DockBuilderFinish(rootId);
}

void Editor::dockNext() {
    if (_rightDockId) {
        ImGui::SetNextWindowDockID(_rightDockId, ImGuiCond_FirstUseEver);
    }
}

} // namespace reone
