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
#include "reone/resource/container/erf.h"
#include "reone/resource/container/rim.h"
#include "reone/resource/format/gffreader.h"
#include "reone/resource/parser/gff/ifo.h"
#include "reone/resource/parser/gff/nfo.h"
#include "reone/system/fileutil.h"
#include "reone/system/stream/fileinput.h"
#include "reone/system/stream/memoryinput.h"
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
#include "reone/scene/registry.h"
#include "reone/scene/render/pipeline.h"
#include "reone/system/stringutil.h"

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder, for the default right-hand layout
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <numeric>

namespace reone {

namespace {

const char *registryMaterialName(graphics::MaterialType type) {
    switch (type) {
    case graphics::MaterialType::OpaqueModel:
        return "OpaqueModel";
    case graphics::MaterialType::TransparentModel:
        return "TransparentModel";
    case graphics::MaterialType::Walkmesh:
        return "Walkmesh";
    case graphics::MaterialType::Grass:
        return "Grass";
    case graphics::MaterialType::Particle:
        return "Particle";
    }
    return "-";
}

/**
 * One character slot per pass, always in the same position.
 *
 * A variable-length list of the passes that drew an entry cannot be read down
 * a column: "D O" and "O" put the O in different places. A fixed layout with a
 * dot for absent turns the column into a bitfield the eye can scan, and the
 * header spells out the order.
 */
std::string registryPassSlots(scene::RenderPassFlags flags) {
    static constexpr scene::RenderPassName kOrder[] {
        scene::RenderPassName::DirLightShadowsPass,
        scene::RenderPassName::PointLightShadows,
        scene::RenderPassName::OpaqueGeometry,
        scene::RenderPassName::TransparentGeometry,
        scene::RenderPassName::PostProcessing,
        scene::RenderPassName::Debug};
    static constexpr char kLabels[] {'D', 'P', 'O', 'T', 'X', 'G'};
    std::string result(std::size(kOrder), '.');
    for (size_t i = 0; i < std::size(kOrder); ++i) {
        if ((flags & scene::renderPassFlag(kOrder[i])) != 0) {
            result[i] = kLabels[i];
        }
    }
    return result;
}

void registryRightAligned(const std::string &text) {
    float width = ImGui::CalcTextSize(text.c_str()).x;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
    }
    ImGui::TextUnformatted(text.c_str());
}

bool containsInsensitive(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                       [](unsigned char a, unsigned char b) {
                           return std::tolower(a) == std::tolower(b);
                       }) != text.end();
}

struct RegistryEntryView {
    const scene::RegisteredObject *object {nullptr};
    const scene::ModelSceneNode *root {nullptr};
    std::string_view modelName;
    std::string_view nodeName;
    const char *kind {""};
    const char *material {"-"};
    scene::RenderPassFlags drawnPasses {0};
    size_t particles {0};
    size_t clusters {0};
    bool debug {false};
    uint32_t id {UINT32_MAX};
    std::string classification;
};

RegistryEntryView makeRegistryEntryView(const scene::ISceneGraph &graph,
                                        const scene::RenderRegistry &registry,
                                        const scene::RegisteredObject &object) {
    return std::visit(
        [&graph, &registry, &object](const auto &entry) -> RegistryEntryView {
            using T = std::decay_t<decltype(entry)>;
            RegistryEntryView result;
            result.object = &object;
            if constexpr (!std::is_same_v<T, scene::RegisteredDebug>) {
                result.id = entry.id.index;
            }
            if constexpr (std::is_same_v<T, scene::RegisteredMesh>) {
                result.root = entry.cullRoot;
                result.modelName = graph.nameText(entry.nameIds.model);
                result.nodeName = graph.nameText(entry.nameIds.node);
                result.material = registryMaterialName(entry.material.type);
                result.drawnPasses = entry.drawnPasses;
                if (std::holds_alternative<scene::RegisteredSkin>(entry.deformation)) {
                    result.kind = "skinned";
                } else if (std::holds_alternative<scene::RegisteredDangly>(entry.deformation)) {
                    result.kind = "dangly";
                } else if (std::holds_alternative<scene::RegisteredSaber>(entry.deformation)) {
                    result.kind = "saber";
                } else {
                    result.kind = "rigid";
                }
                // The traced material classification, matching what the TLAS
                // admission decides: the actual sky room comes from the
                // tracer itself, everything else re-derives from the same
                // authored data the admission reads.
                std::vector<const char *> tags;
                if (entry.cullRoot && entry.cullRoot == registry.skyRoom()) {
                    tags.push_back("sky");
                } else if (entry.material.backgroundGeometry ||
                           (entry.cullRoot && entry.cullRoot->isBackgroundScenery())) {
                    tags.push_back("scenery");
                }
                bool dangly = std::holds_alternative<scene::RegisteredDangly>(entry.deformation);
                if (!dangly &&
                    glm::any(glm::greaterThan(entry.material.selfIllumColor, glm::vec3(0.0f)))) {
                    tags.push_back("emissive");
                }
                if (const auto *diffuse = entry.material.textures[static_cast<size_t>(
                        graphics::MaterialTextureSlot::MainTex)]) {
                    if (diffuse->features().blending == graphics::Texture::Blending::Additive) {
                        tags.push_back("additive");
                    } else if (diffuse->features().blending == graphics::Texture::Blending::PunchThrough ||
                               entry.material.type == graphics::MaterialType::TransparentModel) {
                        tags.push_back("punch-through");
                    }
                }
                for (const auto *tag : tags) {
                    if (!result.classification.empty()) {
                        result.classification += "+";
                    }
                    result.classification += tag;
                }
                if (result.classification.empty()) {
                    result.classification = "-";
                }
                // A curated record supersedes the derived tags on screen,
                // starred so hand-curated rows are visually distinct.
                if (const auto *curated = registry.curatedByIndex(entry.material.curatedIndex)) {
                    switch (curated->klass) {
                    case scene::RenderRegistry::TraceClass::Prelit:
                        result.classification = "prelit*";
                        break;
                    case scene::RenderRegistry::TraceClass::Emissive:
                        result.classification = "emissive*";
                        break;
                    case scene::RenderRegistry::TraceClass::None:
                        result.classification = "none*";
                        break;
                    default:
                        result.classification += "*";
                        break;
                    }
                }
            } else if constexpr (std::is_same_v<T, scene::RegisteredBillboard>) {
                result.root = entry.cullRoot;
                result.modelName = graph.nameText(entry.nameIds.model);
                result.nodeName = graph.nameText(entry.nameIds.node);
                result.kind = "billboard";
                result.drawnPasses = entry.drawnPasses;
            } else if constexpr (std::is_same_v<T, scene::RegisteredParticles>) {
                result.root = entry.cullRoot;
                result.modelName = graph.nameText(entry.nameIds.model);
                result.nodeName = graph.nameText(entry.nameIds.node);
                result.kind = "particles";
                result.material = registryMaterialName(entry.material.type);
                result.drawnPasses = entry.drawnPasses;
                result.particles = entry.instances.size();
            } else if constexpr (std::is_same_v<T, scene::RegisteredGrass>) {
                result.modelName = graph.nameText(entry.nameIds.model);
                result.nodeName = graph.nameText(entry.nameIds.node);
                result.kind = "grass";
                result.material = registryMaterialName(entry.material.type);
                result.drawnPasses = entry.drawnPasses;
                result.clusters = entry.instances.size();
            } else if constexpr (std::is_same_v<T, scene::RegisteredAABB>) {
                result.root = entry.cullRoot;
                result.modelName = graph.nameText(entry.nameIds.model);
                result.nodeName = graph.nameText(entry.nameIds.node);
                result.kind = "aabb";
                result.drawnPasses = entry.drawnPasses;
            } else if constexpr (std::is_same_v<T, scene::RegisteredDebug>) {
                result.kind = "debug";
                result.nodeName = "debug";
                result.debug = true;
            }
            return result;
        },
        object);
}

struct RegistryGroupView {
    const scene::ModelSceneNode *root {nullptr};
    std::string label;
    std::vector<RegistryEntryView> entries;
    size_t particles {0};
    size_t clusters {0};
};

} // namespace

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

void Editor::scanSaves() {
    _saves.clear();
    _savesScanned = true;

    auto savesPath = _engine._options.game.path / "saves";
    std::error_code ec;
    if (!std::filesystem::is_directory(savesPath, ec)) {
        return;
    }
    for (const auto &entry : std::filesystem::directory_iterator(savesPath, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        auto nfoPath = entry.path() / "savenfo.res";
        if (!std::filesystem::exists(nfoPath, ec)) {
            continue;
        }
        try {
            auto stream = reone::FileInputStream(nfoPath);
            resource::GffReader reader(stream);
            reader.load();
            auto nfo = resource::parseNFO(*reader.root());
            SaveEntry save;
            save.directory = entry.path().filename().string();
            save.name = nfo.savegameName;
            save.area = nfo.areaName;
            save.module = nfo.lastModule;
            save.timePlayed = nfo.timePlayed;
            _saves.push_back(std::move(save));
        } catch (const std::exception &e) {
            // A save that cannot be read is worth listing as unreadable rather
            // than silently omitting, but not worth failing the window over.
            SaveEntry save;
            save.directory = entry.path().filename().string();
            save.name = "<unreadable>";
            _saves.push_back(std::move(save));
        }
    }
    std::sort(_saves.begin(), _saves.end(),
              [](const auto &a, const auto &b) { return a.directory < b.directory; });
}

void Editor::scanWarpTargets() {
    _warpTargets.clear();
    _warpTargetsScanned = true;

    auto &game = _engine._game;
    if (!game) {
        return;
    }

    auto modulesPath = findFileIgnoreCase(_engine._options.game.path, "modules");
    if (!modulesPath) {
        return;
    }

    for (const std::string &module : game->moduleNames()) {
        WarpTarget target;
        target.module = module;

        auto readIfo = [&target](auto &container) {
            container.init();
            auto data = container.findResourceData(resource::ResourceId("module", resource::ResType::Ifo));
            if (!data) {
                return;
            }
            auto stream = MemoryInputStream(*data);
            resource::GffReader reader(stream);
            reader.load();
            auto ifo = resource::generated::parseIFO(*reader.root());
            target.title = ifo.Mod_Name.second;
            target.entryArea = ifo.Mod_Entry_Area;
            target.tag = ifo.Mod_Tag;
            target.startMovie = ifo.Mod_StartMovie;
            target.areaCount = ifo.Mod_Area_list.size();
        };

        try {
            if (auto rimPath = findFileIgnoreCase(*modulesPath, module + ".rim")) {
                resource::RimResourceContainer container(*rimPath);
                readIfo(container);
            } else if (auto modPath = findFileIgnoreCase(*modulesPath, module + ".mod")) {
                resource::ErfResourceContainer container(*modPath);
                readIfo(container);
            }
        } catch (const std::exception &) {
            // The module remains warpable; only its optional display metadata
            // is unavailable.
        }
        _warpTargets.push_back(std::move(target));
    }
}

void Editor::warp() {
    dockNext();
    ImGui::SetNextWindowSize(ImVec2(320, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Warp", &_showWarp)) {
        ImGui::End();
        return;
    }
    auto &game = _engine._game;
    if (!game) {
        ImGui::TextUnformatted("No game loaded.");
        ImGui::End();
        return;
    }
    std::string chosen;
    if (!_savesScanned) {
        scanSaves();
    }
    if (!_warpTargetsScanned) {
        scanWarpTargets();
    }

    ImGui::InputTextWithHint("##filter", "Filter targets and saves", _warpFilter, sizeof(_warpFilter));
    ImGui::Separator();
    std::string filter = boost::to_lower_copy(std::string(_warpFilter));

    // Keep warp targets and save games equally visible regardless of window
    // size. Each pane owns its scrolling table and its controls.
    float paneHeight = std::max(0.0f, (ImGui::GetContentRegionAvail().y - ImGui::GetStyle().ItemSpacing.y) * 0.5f);
    if (ImGui::BeginChild("warp targets", ImVec2(0.0f, paneHeight), true)) {
        ImGui::Text("Warp targets (%zu)", _warpTargets.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan targets")) {
            scanWarpTargets();
        }

        auto detailsHeight = 5.0f * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        if (ImGui::BeginTable("targets", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0.0f, -detailsHeight))) {
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Entry area", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Areas", ImGuiTableColumnFlags_WidthFixed, 48.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const auto &target : _warpTargets) {
                if (!filter.empty() &&
                    target.module.find(filter) == std::string::npos &&
                    boost::to_lower_copy(target.title).find(filter) == std::string::npos &&
                    boost::to_lower_copy(target.entryArea).find(filter) == std::string::npos &&
                    boost::to_lower_copy(target.tag).find(filter) == std::string::npos) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string &label = target.title.empty() ? target.module : target.title;
                bool selected = _selectedWarp == target.module;
                // Titles such as "Dantooine" are shared by several module
                // archives, so the resref must provide the widget identity.
                ImGui::PushID(target.module.c_str());
                if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    _selectedWarp = target.module;
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(target.module.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(target.entryArea.empty() ? "-" : target.entryArea.c_str());
                ImGui::TableNextColumn();
                if (target.areaCount != 0) {
                    ImGui::Text("%zu", target.areaCount);
                } else {
                    ImGui::TextUnformatted("-");
                }
            }
            ImGui::EndTable();
        }

        auto selected = std::find_if(_warpTargets.begin(), _warpTargets.end(), [this](const auto &target) {
            return target.module == _selectedWarp;
        });
        ImGui::Separator();
        if (selected == _warpTargets.end()) {
            ImGui::TextDisabled("Select a target to inspect its level metadata.");
        } else {
            ImGui::Text("Target: %s", selected->module.c_str());
            ImGui::Text("Entry area: %s", selected->entryArea.empty() ? "unavailable" : selected->entryArea.c_str());
            ImGui::Text("Tag: %s", selected->tag.empty() ? "unavailable" : selected->tag.c_str());
            ImGui::Text("Start movie: %s", selected->startMovie.empty() ? "none" : selected->startMovie.c_str());
            if (ImGui::Button("Warp to selected")) {
                chosen = selected->module;
            }
        }
    }
    ImGui::EndChild();

    std::string chosenSave;
    if (ImGui::BeginChild("saved games", ImVec2(0.0f, paneHeight), true)) {
        ImGui::Text("Saved games (%zu)", _saves.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan saves")) {
            scanSaves();
        }
        if (ImGui::BeginTable("saves", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0.0f, 0.0f))) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Area");
            ImGui::TableSetupColumn("Module");
            ImGui::TableSetupColumn("Played");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const auto &save : _saves) {
                if (!filter.empty() &&
                    boost::to_lower_copy(save.name).find(filter) == std::string::npos &&
                    boost::to_lower_copy(save.area).find(filter) == std::string::npos &&
                    boost::to_lower_copy(save.module).find(filter) == std::string::npos &&
                    boost::to_lower_copy(save.directory).find(filter) == std::string::npos) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                // Save names are user-editable and can also repeat.
                ImGui::PushID(save.directory.c_str());
                if (ImGui::Selectable(save.name.empty() ? save.directory.c_str() : save.name.c_str(),
                                      false, ImGuiSelectableFlags_SpanAllColumns)) {
                    chosenSave = save.directory;
                }
                ImGui::PopID();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", save.directory.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(save.area.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(save.module.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u:%02u:%02u", save.timePlayed / 3600,
                            (save.timePlayed / 60) % 60, save.timePlayed % 60);
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::End();

    if (!chosen.empty()) {
        _pendingWarp = chosen;
    }
    if (!chosenSave.empty()) {
        _pendingLoadGame = chosenSave;
    }
}

void Editor::pathTracingSettings() {
    dockNext();
    ImGui::SetNextWindowSize(ImVec2(360, 300), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Path tracing", &_showPathTracing)) {
        ImGui::End();
        return;
    }
    auto &options = _engine._options.graphics;
    if (options.mode != "path-tracing") {
        ImGui::TextDisabled("Inactive - run with --mode path-tracing.");
        ImGui::TextDisabled("Settings still save and apply when it is.");
        ImGui::Separator();
    }
    // Everything here rides in push constants, so a change applies on the next
    // frame with nothing rebuilt. These are deliberately not command-line
    // options: they are tuning dials, and the place to turn a dial is next to
    // the picture it changes.
    if (ImGui::SliderInt("Samples per pixel", &options.pathTracingSamples, 1, 64)) {
        options.pathTracingSamples = std::max(1, options.pathTracingSamples);
    }
    ImGui::TextDisabled("Cost is near linear; noise falls as sqrt.");
    if (ImGui::SliderInt("Bounces", &options.ptBounces, 1, 8)) {
        options.ptBounces = std::clamp(options.ptBounces, 1, 8);
    }
    ImGui::TextDisabled("Path depth after the primary hit. Deeper paths\ncarry light around corners; the lightmap cache\nalready answers much of it on static geometry.");
    ImGui::SeparatorText("Source intensities");
    ImGui::SliderFloat("Sky", &options.ptSkyIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Emissive", &options.ptEmissiveIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Lightmap cache", &options.ptLightmapIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Direct light", &options.ptDirectIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Sun", &options.ptSunIntensity, 0.0f, 4.0f, "%.2f");
    ImGui::SeparatorText("Ray setup");
    ImGui::SliderFloat("Origin offset", &options.ptRayOffset, 0.0001f, 0.1f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::TextDisabled("Too small: acne and black speckling.\nToo large: light leaks at contact edges.");
    ImGui::SeparatorText("Display");
    static constexpr const char *kTonemapNames[] = {"Off (linear)", "ACES"};
    ImGui::Combo("Tonemap", &options.ptTonemap, kTonemapNames, 2);
    ImGui::SliderFloat("Exposure", &options.ptExposure, 0.05f, 8.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SeparatorText("Light shape");
    ImGui::SliderFloat("Point angular size", &options.ptPointAngularSize, 0.05f, 45.0f, "%.1f deg");
    ImGui::SliderFloat("Sun angular size", &options.ptSunAngularSize, 0.05f, 10.0f, "%.2f deg");
    ImGui::TextDisabled("A light source is never a point. Wide points give\nsoft penumbras; the sun stays fairly sharp.");
    ImGui::SeparatorText("Debug view");
    static constexpr const char *kDebugViewNames[] = {
        "Off", "Object categories", "Emissive highlight", "Normals",
        "Roughness", "Lightmap", "Albedo",
        "Denoiser: diffuse channel", "Denoiser: specular channel",
        "Denoiser: viewZ", "Denoiser: noise-free"};
    ImGui::Combo("View", &options.ptDebugView, kDebugViewNames, 11);
    ImGui::TextDisabled("Replaces shading at the primary hit. Categories:\nblue rooms, red creatures, green placeables,\nmagenta doors, yellow equipment, cyan sky.\nDenoiser views show the NRD output split.");
    ImGui::SeparatorText("Category overrides");
    static constexpr const char *kCategoryNames[] = {
        "GUI", "Rooms", "Creatures", "Placeables", "Doors",
        "Equipment", "Projectiles", "Cameras", "Uncategorized"};
    for (int i = 0; i < 9; ++i) {
        // GUI, projectile and camera models never reach the TLAS.
        if (i == 0 || i == 6 || i == 7) {
            continue;
        }
        auto &override = options.ptCategoryOverrides[i];
        if (ImGui::TreeNode(kCategoryNames[i])) {
            ImGui::ColorEdit3("Color", override.color);
            ImGui::SliderFloat("Color weight", &override.colorWeight, 0.0f, 1.0f, "%.2f");
            bool overrideRoughness = override.roughness >= 0.0f;
            if (ImGui::Checkbox("Override roughness", &overrideRoughness)) {
                override.roughness = overrideRoughness ? 0.5f : -1.0f;
            }
            if (overrideRoughness) {
                ImGui::SliderFloat("Roughness", &override.roughness, 0.0f, 1.0f, "%.2f");
            }
            ImGui::SliderFloat("Emission scale", &override.emissionScale, 0.0f, 8.0f, "%.2f");
            ImGui::SliderFloat("Env strength scale", &override.envScale, 0.0f, 4.0f, "%.2f");
            ImGui::TreePop();
        }
    }
    ImGui::TextDisabled("Applied at trace time to every surface of the category.\nColor weight 1 flat-paints for bug isolation.");
    ImGui::SeparatorText("Diagnostics");
    ImGui::Checkbox("Trace stats", &options.ptTraceStats);
    ImGui::TextDisabled("GPU counters in the engine log. Costs frame time;\nleave off when measuring.");
    ImGui::End();
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

void Editor::drawRegistry() {
    dockNext();
    ImGui::SetNextWindowSize(ImVec2(700, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Registry", &_showRegistry, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    auto &graphs = _engine._sceneModule->graphs();
    auto sceneNames = graphs.sceneNames();
    if (sceneNames.empty()) {
        ImGui::TextUnformatted("No scenes registered.");
        ImGui::End();
        return;
    }
    if (_registryScene.empty() || sceneNames.count(_registryScene) == 0) {
        auto main = sceneNames.find(game::kSceneMain);
        _registryScene = main != sceneNames.end() ? *main : *sceneNames.begin();
    }
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("##registry-scene", _registryScene.c_str())) {
        for (const auto &name : sceneNames) {
            if (ImGui::Selectable(name.c_str(), name == _registryScene)) {
                _registryScene = name;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("scene");

    auto &graph = graphs.get(_registryScene);
    // Editor::update runs before SceneGraph::render resets and fills the
    // registry, so this is deliberately the previous frame's complete
    // snapshot. Reading it during render would expose a partial frame.
    auto &registry = graph.registry();
    const auto &registered = registry.registeredCounts();
    const auto &drawnByPass = registry.drawnCountsByPass();
    auto drawnEntries = [&drawnByPass](scene::RenderPassName pass) {
        auto it = drawnByPass.find(pass);
        return it == drawnByPass.end() ? size_t {0} : it->second.entries;
    };
    ImGui::Text("registered %zu", registered.entries);
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::Text("drawn  opaque %zu   transparent %zu   dir shadows %zu",
                drawnEntries(scene::RenderPassName::OpaqueGeometry),
                drawnEntries(scene::RenderPassName::TransparentGeometry),
                drawnEntries(scene::RenderPassName::DirLightShadowsPass));

    float controlsRight = ImGui::CalcTextSize("Hide fully culled").x + ImGui::GetFrameHeight() +
                          ImGui::GetStyle().ItemSpacing.x * 3.0f;
    ImGui::SetNextItemWidth(-controlsRight);
    ImGui::InputTextWithHint("##registry-filter", "Filter model or node", _registryFilter, sizeof(_registryFilter));
    ImGui::SameLine();
    ImGui::Checkbox("Hide fully culled", &_registryHideFullyCulled);
    ImGui::TextDisabled("DPOTXG: dir shadows, point shadows, opaque, transparent, post, debug");
    ImGui::Spacing();

    std::vector<RegistryGroupView> groups;
    for (const auto &object : registry.objects()) {
        auto entry = makeRegistryEntryView(graph, registry, object);
        std::string label;
        if (entry.root) {
            label = std::string(graph.nameText(entry.root->nameIds().model));
            if (label.empty()) {
                label = "[unnamed model]";
            }
        } else if (entry.debug) {
            label = "[debug]";
        } else if (entry.kind == std::string_view("grass")) {
            label = "[grass]";
        } else if (entry.kind == std::string_view("particles")) {
            label = "[emitters]";
        } else {
            label = "[rootless]";
        }
        auto group = std::find_if(groups.begin(), groups.end(), [&](const auto &candidate) {
            return candidate.root == entry.root && candidate.label == label;
        });
        if (group == groups.end()) {
            groups.push_back({entry.root, std::move(label)});
            group = std::prev(groups.end());
        }
        group->particles += entry.particles;
        group->clusters += entry.clusters;
        group->entries.push_back(std::move(entry));
    }
    std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) {
        return a.label < b.label;
    });
    // A resref names a model, not an instance, and an area places the same one
    // many times - danm14ab has eight c_khounda. Groups are already distinct,
    // being keyed on the root node, but on screen they were eight identical
    // rows. Disambiguate only where it is needed, so the common case stays
    // clean.
    for (size_t i = 0; i < groups.size();) {
        size_t j = i;
        while (j < groups.size() && groups[j].label == groups[i].label) {
            ++j;
        }
        if (j - i > 1) {
            for (size_t k = i; k < j; ++k) {
                if (groups[k].root) {
                    groups[k].label += " #" + std::to_string(groups[k].root->id().index);
                }
            }
        }
        i = j;
    }

    // A table rather than formatted text: entry rows are the thing being
    // compared against each other, and columns are what make them comparable.
    // Padded text drifts as soon as a name is longer than the padding.
    // ScrollX with every column fixed, rather than letting the name column
    // stretch. Docked narrow, a stretch column is what gives way first, and
    // the names are the one thing the panel exists to show - a 480px dock
    // collapsed them to nothing while the count columns kept their width.
    // Scrolling is the honest response to a window too small for the data.
    static constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##registry-table", 8, kTableFlags)) {
        ImGui::End();
        return;
    }
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 26.0f);
    ImGui::TableSetupColumn("Model / node", ImGuiTableColumnFlags_WidthFixed, 250.0f);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 64.0f);
    ImGui::TableSetupColumn("DPOTXG", ImGuiTableColumnFlags_WidthFixed, 66.0f);
    ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthFixed, 158.0f);
    ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    ImGui::TableSetupColumn("Entries", ImGuiTableColumnFlags_WidthFixed, 68.0f);
    ImGui::TableSetupColumn("Drawn", ImGuiTableColumnFlags_WidthFixed, 58.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    std::string_view filter(_registryFilter);
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        auto &group = groups[groupIndex];
        std::vector<const RegistryEntryView *> visible;
        visible.reserve(group.entries.size());
        for (const auto &entry : group.entries) {
            if (containsInsensitive(entry.modelName, filter) ||
                containsInsensitive(entry.nodeName, filter)) {
                visible.push_back(&entry);
            }
        }
        if (visible.empty()) {
            continue;
        }
        const auto drawn = std::count_if(visible.begin(), visible.end(), [](const auto *entry) {
            return entry->drawnPasses != 0;
        });
        if (_registryHideFullyCulled && drawn == 0) {
            continue;
        }

        ImGui::PushID(static_cast<int>(groupIndex));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        // The kill switch: unticking removes the object from every render
        // mode - raster walks and the TLAS alike. The group box drives all
        // of its entries at once.
        bool groupEnabled = std::all_of(visible.begin(), visible.end(), [&](const auto *entry) {
            return entry->debug || registry.isObjectEnabled(entry->id);
        });
        if (ImGui::Checkbox("##group-on", &groupEnabled)) {
            for (const auto *entry : visible) {
                if (!entry->debug) {
                    registry.setObjectEnabled(entry->id, groupEnabled);
                }
            }
        }
        ImGui::TableSetColumnIndex(1);
        const bool open = ImGui::TreeNodeEx(group.label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);

        // The instance counts belong on the group, not on its rows: grass is one
        // entry holding 1482 clusters, so a per-entry column would read "1" and
        // hide the only number that matters for it.
        if (group.clusters != 0 || group.particles != 0) {
            ImGui::TableSetColumnIndex(4);
            ImGui::TextDisabled("%zu %s",
                                group.clusters != 0 ? group.clusters : group.particles,
                                group.clusters != 0 ? "clusters" : "particles");
        }
        ImGui::TableSetColumnIndex(6);
        registryRightAligned(std::to_string(visible.size()));
        ImGui::TableSetColumnIndex(7);
        if (drawn == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            registryRightAligned("0");
            ImGui::PopStyleColor();
        } else {
            registryRightAligned(std::to_string(drawn));
        }

        if (open) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto &entry = *visible[i];
                    ImGui::PushID(static_cast<int>(i));
                    const std::string_view node = entry.nodeName.empty()
                                                      ? std::string_view("[unnamed]")
                                                      : entry.nodeName;
                    // A culled entry is dimmed rather than annotated, so the
                    // eye finds the drawn ones without reading every row.
                    const bool culled = entry.drawnPasses == 0;
                    if (culled) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    }
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (!entry.debug) {
                        bool enabled = registry.isObjectEnabled(entry.id);
                        if (ImGui::Checkbox("##on", &enabled)) {
                            registry.setObjectEnabled(entry.id, enabled);
                        }
                    }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(node.data(), node.data() + node.size());
                    // Right-click classifies: the mechanical per-level pass,
                    // written straight to trace-classes.txt.
                    if (!entry.debug && ImGui::BeginPopupContextItem("##classify")) {
                        auto model = std::string(entry.modelName);
                        auto nodeName2 = std::string(entry.nodeName);
                        auto curated = registry.curatedFor(model, nodeName2);
                        auto item = [&](const char *label, scene::RenderRegistry::TraceClass klass) {
                            if (ImGui::MenuItem(label, nullptr, curated.klass == klass)) {
                                curated.klass = curated.klass == klass
                                                    ? scene::RenderRegistry::TraceClass::Default
                                                    : klass;
                                registry.setCurated(model, nodeName2, curated);
                            }
                        };
                        ImGui::TextDisabled("Classify");
                        ImGui::Separator();
                        item("Prelit (fullbright, casts nothing)", scene::RenderRegistry::TraceClass::Prelit);
                        item("Emissive (glows and casts)", scene::RenderRegistry::TraceClass::Emissive);
                        item("None (strip selfIllum)", scene::RenderRegistry::TraceClass::None);
                        ImGui::Separator();
                        if (ImGui::MenuItem("Edit material...")) {
                            _showMaterialEditor = true;
                            _materialEditModel = model;
                            _materialEditNode = nodeName2;
                            _materialEdit = curated;
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(entry.kind);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(registryPassSlots(entry.drawnPasses).c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(entry.material);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(entry.classification.c_str());
                    if (culled) {
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    ImGui::EndTable();
    ImGui::End();

    drawMaterialEditor(registry);
}

void Editor::drawMaterialEditor(scene::RenderRegistry &registry) {
    if (!_showMaterialEditor) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(380, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Curated material", &_showMaterialEditor)) {
        ImGui::End();
        return;
    }
    ImGui::Text("%s / %s", _materialEditModel.c_str(), _materialEditNode.c_str());
    ImGui::TextDisabled("Every change applies live and saves to trace-classes.txt.");
    ImGui::Separator();

    bool changed = false;
    static constexpr const char *kClassNames[] = {"Default", "Prelit", "Emissive", "None"};
    int klass = static_cast<int>(_materialEdit.klass);
    if (ImGui::Combo("Class", &klass, kClassNames, 4)) {
        _materialEdit.klass = static_cast<scene::RenderRegistry::TraceClass>(klass);
        changed = true;
    }

    ImGui::SeparatorText("Albedo");
    changed |= ImGui::DragFloat3("Multiplier", &_materialEdit.albedoMul.x, 0.01f, 0.0f, 4.0f, "%.2f");

    // Both derived channels share the mode ladder: leave the heuristic
    // alone, override with a constant, or drive it from albedo with
    // lerp(base, smoothstep(a, b, dot(albedo, weights)), t).
    static constexpr const char *kModeNames[] = {"Derived (default)", "Constant", "Albedo curve"};
    auto channel = [&changed](const char *label, int &mode, glm::vec4 &params, glm::vec3 &weights) {
        ImGui::PushID(label);
        ImGui::SeparatorText(label);
        changed |= ImGui::Combo("Mode", &mode, kModeNames, 3);
        if (mode == 1) {
            changed |= ImGui::SliderFloat("Value", &params.x, 0.0f, 1.0f, "%.2f");
        } else if (mode == 2) {
            changed |= ImGui::SliderFloat("Base", &params.x, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Edge a", &params.y, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Edge b", &params.z, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::SliderFloat("Blend t", &params.w, 0.0f, 1.0f, "%.2f");
            changed |= ImGui::DragFloat3("Weights", &weights.x, 0.01f, 0.0f, 1.0f, "%.2f");
        }
        ImGui::PopID();
    };
    channel("Roughness", _materialEdit.roughnessMode, _materialEdit.roughnessParams,
            _materialEdit.roughnessWeights);
    channel("Metallic", _materialEdit.metallicMode, _materialEdit.metallicParams,
            _materialEdit.metallicWeights);

    ImGui::SeparatorText("Emission");
    static constexpr const char *kEmissionModes[] = {"Untouched", "Multiplier", "Override"};
    changed |= ImGui::Combo("Emission mode", &_materialEdit.emissionMode, kEmissionModes, 3);
    if (_materialEdit.emissionMode != 0) {
        changed |= ImGui::DragFloat3("Emission value", &_materialEdit.emissionValue.x, 0.02f,
                                     0.0f, 16.0f, "%.2f");
    }

    ImGui::Separator();
    if (ImGui::Button("Reset to default")) {
        _materialEdit = scene::RenderRegistry::CuratedMaterial();
        changed = true;
    }
    if (changed) {
        registry.setCurated(_materialEditModel, _materialEditNode, _materialEdit);
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

void Editor::applyPendingTransition() {
    if (!_pendingWarp.empty()) {
        auto target = std::move(_pendingWarp);
        _pendingWarp.clear();
        // The render-target viewer keeps a raw scene texture pointer. Loading
        // a module destroys that scene before its loading screen presents.
        _rtSource = nullptr;
        if (_engine._game) {
            _engine._game->loadModule(target);
        }
        return;
    }
    if (!_pendingLoadGame.empty()) {
        auto target = std::move(_pendingLoadGame);
        _pendingLoadGame.clear();
        _rtSource = nullptr;
        if (_engine._game) {
            _engine._game->loadGame(target);
        }
        return;
    }
}

void Editor::update(float dt) {
    if (!_enabled) {
        return;
    }

    (void)dt;

    // Submitted before the dockspace so the viewport work area excludes it.
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Tools")) {
            ImGui::MenuItem("2DA", nullptr, &_showTwoDa);
            ImGui::MenuItem("Registry", nullptr, &_showRegistry);
            ImGui::MenuItem("Render targets", nullptr, &_showRenderTargets);
            ImGui::MenuItem("Graphics settings", nullptr, &_showGraphicsSettings);
            ImGui::MenuItem("Path tracing", nullptr, &_showPathTracing);
            ImGui::MenuItem("Warp", nullptr, &_showWarp);
            ImGui::MenuItem("Frame times", nullptr, &_showFrameTimes);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Debug")) {
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

    if (_showRegistry) {
        drawRegistry();
    }

    if (_showWarp) {
        warp();
    }
    if (_showGraphicsSettings) {
        graphicsSettings();
    }
    if (_showPathTracing) {
        pathTracingSettings();
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
