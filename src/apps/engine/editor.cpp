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
#include <map>

#include "engine.h"

#include "reone/game/debug.h"
#include "reone/game/types.h"
#include "reone/graphics/di/services.h"
#include "reone/graphics/mesh.h"
#include "reone/graphics/meshregistry.h"
#include "reone/graphics/optionsregistry.h"
#include "reone/graphics/textureutil.h"
#include "reone/resource/container/erf.h"
#include "reone/resource/container/rim.h"
#include "reone/resource/format/gffreader.h"
#include "reone/resource/parser/gff/ifo.h"
#include "reone/resource/parser/gff/nfo.h"
#include "reone/resource/resources.h"
#include "reone/scene/gpuscene.h"
#include "reone/scene/graph.h"
#include "reone/scene/graphs.h"
#include "reone/scene/render/pipeline.h"
#include "reone/graphics/format/tgawriter.h"
#include "reone/system/fileutil.h"
#include "reone/system/stream/fileinput.h"
#include "reone/system/stream/fileoutput.h"
#include "reone/system/stream/memoryinput.h"
#include "reone/system/stringutil.h"

#include <SDL3/SDL_filesystem.h>

#include <filesystem>
#include <system_error>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder, for the default right-hand layout
#include "imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>

namespace reone {

namespace {

constexpr char kConfigFilename[] = "reone.cfg";

std::string formatConfigFloat(float value) {
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return stream.str();
}

bool saveGraphicsOptions(const graphics::GraphicsOptions &options, std::string &error) {
    // Every option the registry knows, asked for its own written form.
    //
    // This was a hand-maintained literal and it had drifted: 19 of the
    // registry's 114 options were absent, so changing one of them in this
    // window and pressing Save wrote nothing and still reported success -
    // indistinguishable from a save that does not work, which is how it was
    // reported. Among the missing were every gui*scale, shadows, particles,
    // lightmaps and several path-tracing dials.
    //
    // Driving it from the registry cannot drift: an option that exists is
    // written, because the descriptor that defines it also supplies its value.
    // The command line and reone.cfg already read through this same list, so
    // all three now agree on what an option is called and how it is spelled.
    std::vector<std::pair<std::string, std::string>> values;
    values.reserve(graphics::graphicsOptionDescs().size());
    for (const auto &desc : graphics::graphicsOptionDescs()) {
        // headless is a launch mode, not a preference: writing it would let a
        // scripted capture run leave the next interactive start windowless.
        if (desc.name == "headless") {
            continue;
        }
        values.emplace_back(desc.name, desc.get(options));
    }

    for (int i = 0; i < 9; ++i) {
        const auto &override = options.categoryOverrides[i];
        auto key = "cat" + std::to_string(i);
        values.emplace_back(key + "color0", formatConfigFloat(override.color[0]));
        values.emplace_back(key + "color1", formatConfigFloat(override.color[1]));
        values.emplace_back(key + "color2", formatConfigFloat(override.color[2]));
        values.emplace_back(key + "colorweight", formatConfigFloat(override.colorWeight));
        values.emplace_back(key + "roughness", formatConfigFloat(override.roughness));
        values.emplace_back(key + "roughnessscale", formatConfigFloat(override.roughnessScale));
        values.emplace_back(key + "emission", formatConfigFloat(override.emissionScale));
        values.emplace_back(key + "env", formatConfigFloat(override.envScale));
        values.emplace_back(key + "metallic", formatConfigFloat(override.metallicScale));
    }

    std::ifstream input(kConfigFilename);
    if (!input && std::filesystem::exists(kConfigFilename)) {
        error = "Could not read reone.cfg.";
        return false;
    }

    // Launcher, game and editor settings share this file, so only owned keys
    // are replaced while every foreign line keeps its position and contents.
    std::vector<std::string> lines;
    std::set<std::string> written;
    for (std::string line; std::getline(input, line);) {
        auto separator = line.find('=');
        auto key = separator == std::string::npos ? std::string() : line.substr(0, separator);
        auto value = std::find_if(values.begin(), values.end(), [&key](const auto &entry) { return entry.first == key; });
        if (value == values.end()) {
            lines.push_back(std::move(line));
        } else if (written.insert(key).second) {
            lines.push_back(value->first + "=" + value->second);
        }
    }

    for (const auto &[key, value] : values) {
        if (written.insert(key).second) {
            lines.push_back(key + "=" + value);
        }
    }

    std::ofstream output(kConfigFilename);
    if (!output) {
        error = "Could not write reone.cfg.";
        return false;
    }
    for (const auto &line : lines) {
        output << line << '\n';
    }
    if (!output) {
        error = "Could not finish writing reone.cfg.";
        return false;
    }
    return true;
}

const char *objectMaterialName(graphics::MaterialType type) {
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

void objectRightAligned(const std::string &text) {
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

struct ObjectEntryView {
    const scene::ObjectRecord *object {nullptr};
    const scene::ModelSceneNode *root {nullptr};
    std::string_view modelName;
    std::string_view nodeName;
    const char *kind {""};
    const char *material {"-"};
    size_t particles {0};
    size_t clusters {0};
    uint32_t id {UINT32_MAX};
    std::string classification;
};

ObjectEntryView makeObjectEntryView(const scene::ISceneGraph &graph,
                                    const scene::GpuScene &scene,
                                    const scene::ObjectRecord &object) {
    return std::visit(
        [&graph, &scene, &object](const auto &entry) -> ObjectEntryView {
            using T = std::decay_t<decltype(entry)>;
            ObjectEntryView result;
            result.object = &object;
            result.id = entry.id.index;
            if constexpr (std::is_same_v<T, scene::RegisteredMesh>) {
                result.root = entry.cullRoot;
                result.modelName = graph.nameText(entry.nameIds.model);
                result.nodeName = graph.nameText(entry.nameIds.node);
                result.material = objectMaterialName(entry.material.type);
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
                if (entry.cullRoot && entry.cullRoot == scene.skyRoom()) {
                    tags.push_back("sky");
                } else if (entry.material.backgroundGeometry ||
                           (entry.cullRoot && entry.cullRoot->isBackgroundScenery())) {
                    tags.push_back("scenery");
                }
                bool dangly = std::holds_alternative<scene::RegisteredDangly>(entry.deformation);
                // Doors are stripped by admission for being doors, so tagging
                // one emissive here would name a class the tracer never gave it.
                bool door = scene::isDoorMesh(entry);
                if (!dangly && !door &&
                    glm::any(glm::greaterThan(entry.material.selfIllumColor, glm::vec3(0.0f)))) {
                    tags.push_back("emissive");
                }
                if (const auto *diffuse = entry.material.textures[static_cast<size_t>(
                        graphics::MaterialTextureSlot::MainTex)]) {
                    if (diffuse->features().blending == graphics::Texture::Blending::Additive) {
                        tags.push_back("additive");
                    } else if (diffuse->features().blending == graphics::Texture::Blending::PunchThrough) {
                        tags.push_back("punch-through");
                    } else if (entry.material.type == graphics::MaterialType::TransparentModel) {
                        tags.push_back("alpha-blended");
                    }
                } else if (entry.material.type == graphics::MaterialType::TransparentModel) {
                    tags.push_back("alpha-blended");
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
                if (const auto *curated =
                        scene.traceMaterials().curatedByIndex(entry.material.curatedIndex)) {
                    switch (curated->klass) {
                    case scene::TraceClass::Prelit:
                        result.classification = "prelit*";
                        break;
                    case scene::TraceClass::Emissive:
                        result.classification = "emissive*";
                        break;
                    case scene::TraceClass::None:
                        result.classification = "none*";
                        break;
                    default:
                        result.classification += "*";
                        break;
                    }
                }
            } else if constexpr (std::is_same_v<T, scene::RegisteredProcedural>) {
                result.root = entry.cullRoot;
                result.modelName = graph.nameText(entry.nameIds.model);
                result.nodeName = graph.nameText(entry.nameIds.node);
                result.material = objectMaterialName(entry.material.type);
                switch (entry.kind) {
                case scene::ProceduralKind::Grass:
                    result.kind = "grass";
                    result.clusters = entry.instanceCount();
                    break;
                case scene::ProceduralKind::Particles:
                    result.kind = "particles";
                    result.particles = entry.instances.size();
                    break;
                case scene::ProceduralKind::Billboard:
                    result.kind = "billboard";
                    break;
                }
            }
            return result;
        },
        object);
}

struct ObjectGroupView {
    const scene::ModelSceneNode *root {nullptr};
    std::string label;
    std::vector<ObjectEntryView> entries;
    size_t particles {0};
    size_t clusters {0};
};

/**
 * The explanation for the control just submitted, shown on hover.
 *
 * These were paragraphs printed under every dial, which made the settings
 * window mostly prose: the reasoning is wanted when a dial is in question and
 * is noise the rest of the time. The text is unchanged, only relocated.
 *
 * Written against IsItemHovered rather than SetItemTooltip because a wrap
 * position has to be pushed inside the tooltip window - the paragraphs are
 * long, and an unwrapped tooltip is one line as wide as the screen. Disabled
 * controls do not report hover unless asked to, and two of them (FSR sharpness,
 * the restart-only pair) carry the explanation of why they are disabled, which
 * is exactly when it is wanted.
 */
void settingHint(const char *text, bool whenDisabled = false) {
    ImGuiHoveredFlags flags = ImGuiHoveredFlags_ForTooltip;
    if (whenDisabled) {
        flags |= ImGuiHoveredFlags_AllowWhenDisabled;
    }
    if (!ImGui::IsItemHovered(flags) || !ImGui::BeginTooltip()) {
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

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

void Editor::graphicsSettings() {
    dockNext();
    ImGui::SetNextWindowSize(ImVec2(520, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics settings", &_showGraphicsSettings)) {
        ImGui::End();
        return;
    }

    // The commit row is pinned to the bottom of the window rather than left at
    // the end of the flow. A staged control can be in any tab - the
    // anti-aliasing slot is in Renderer, the resolution in Quality - and the
    // one button that commits them all has to be reachable without knowing
    // which tab scrolled it away. Two text lines are reserved under the row for
    // the pending list and the save result.
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() +
                               2.0f * ImGui::GetTextLineHeightWithSpacing() +
                               ImGui::GetStyle().ItemSpacing.y;
    // The tab bar stays outside the scrolling region. It is navigation, not a
    // setting: on a long tab it would otherwise scroll out of reach and leave no
    // way back to another tab without scrolling up first.
    auto tab = [this, footerHeight](const char *label, void (Editor::*body)()) {
        if (!ImGui::BeginTabItem(label)) {
            return;
        }
        if (ImGui::BeginChild("##scroll", ImVec2(0.0f, -footerHeight))) {
            // Labels sit to the right of their widget, so a widget allowed to
            // take the full width clips its own label. Half the row leaves
            // room for the longest of them ("Bounce roughness") at the
            // width this window is usually docked to.
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
            (this->*body)();
            ImGui::PopItemWidth();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    };
    // Ordered by how often a dial is reached for: what changes the picture
    // first, then what it costs, then the tracer, then the dials that only a
    // specific artefact sends you looking for.
    if (ImGui::BeginTabBar("##graphics-tabs")) {
        tab("Renderer", &Editor::graphicsRendererTab);
        tab("Quality", &Editor::graphicsQualityTab);
        tab("Path tracing", &Editor::graphicsPathTracingTab);
        tab("Advanced", &Editor::graphicsAdvancedTab);
        tab("Materials", &Editor::graphicsMaterialsTab);
        ImGui::EndTabBar();
    }

    graphicsCommitFooter();
    ImGui::End();
}

void Editor::graphicsRendererTab() {
    auto &options = _engine._options.graphics;
    auto &staged = _engine.stagedGraphicsOptions();

    renderModeCombo();

    ImGui::SeparatorText("Post-process features");
    ImGui::Checkbox("Lens flares", &options.lensFlares);
    settingHint("Authored light halos, drawn through the shared blended tail in every mode.");

    ImGui::Checkbox("Bloom", &options.bloom);
    settingHint("Blurs emissive highlights before transparency in every mode.");
    ImGui::BeginDisabled(!options.bloom);
    ImGui::SliderFloat("Bloom threshold", &options.bloomThreshold, 0.0f, 4.0f, "%.2f");
    settingHint("Display-space level an emissive texel must pass before it blooms.", true);
    ImGui::SliderFloat("Bloom intensity", &options.bloomIntensity, 0.0f, 4.0f, "%.2f");
    settingHint("Scale on the blurred highlights added back to the frame.", true);
    ImGui::EndDisabled();

    // One slot with one occupant, the same in every mode. The choice is staged
    // rather than live: FSR builds a context and device images in the
    // pipeline's init, so it cannot be switched inside a frame.
    // Ordered as the enum is, so the index is the value.
    static const char *kAntiAliasingNames[] = {"Off", "FXAA", "FSR 2 (NativeAA)",
                                               "DLSS Ray Reconstruction"};
    static_assert(IM_ARRAYSIZE(kAntiAliasingNames) ==
                      static_cast<int>(graphics::AntiAliasing::DlssRr) + 1,
                  "every AntiAliasing value needs a name: the array is indexed by the enum "
                  "below, so a missing entry is an out-of-bounds read rather than a gap in "
                  "the combo");
    int antiAliasing = static_cast<int>(staged.antialiasing);
    if (ImGui::Combo("Anti-aliasing", &antiAliasing, kAntiAliasingNames,
                     IM_ARRAYSIZE(kAntiAliasingNames))) {
        staged.antialiasing = static_cast<graphics::AntiAliasing>(antiAliasing);
    }
    settingHint("Runs after the opaque resolve, before transparency and the display transform. "
                "It never tonemaps. Apply is at the bottom of this window.");
    if (staged.antialiasing != options.antialiasing) {
        // Staged options are invisible until Apply, and this one has a visible
        // consequence: jitter follows the resolver, so a pending FSR-to-FXAA
        // change leaves the projection jittering while the combo already reads
        // FXAA. Naming what is actually running is the difference between a
        // change that is pending and one that looks broken. Inline rather than
        // on hover for the same reason: it is a state, not an explanation.
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Running: %s until Apply.",
                           kAntiAliasingNames[static_cast<int>(options.antialiasing)]);
    }
    // What is actually resolving, and for DLSS what version of it. The slot's
    // occupant is the one graphics choice whose real behaviour cannot be read
    // off the combo: DLSS falls back to FSR when the DLLs or the GPU are not
    // there, and it does so silently by design. Naming the running resolver -
    // and its version, which the user can change by dropping in a different
    // nvngx_dlssd.dll without rebuilding - is what makes that legible.
    const bool dlssSelected = staged.antialiasing == graphics::AntiAliasing::DlssRr ||
                              options.antialiasing == graphics::AntiAliasing::DlssRr;
    const bool dlssAvailable = _engine._graphicsModule->renderer().rayReconstructionAvailable();
    if (dlssSelected) {
        if (dlssAvailable) {
            graphics::RayReconstructionInfo info =
                _engine._graphicsModule->renderer().rayReconstructionInfo();
            ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f),
                               "DLSS Ray Reconstruction   Streamline %u.%u.%u   NGX %u.%u.%u",
                               info.streamlineMajor, info.streamlineMinor, info.streamlineBuild,
                               info.ngxMajor, info.ngxMinor, info.ngxBuild);
            settingHint("The neural model lives in nvngx_dlssd.dll beside the executable. "
                        "Replacing that file with a newer one changes the NGX version here "
                        "and the picture with it; nothing needs rebuilding.",
                        true);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                               "DLSS Ray Reconstruction: NOT available - FSR runs instead.");
            settingHint("Needs sl.interposer.dll, sl.common.dll, sl.dlss_d.dll and "
                        "nvngx_dlssd.dll beside the executable, and a GeForce RTX GPU. "
                        "None of them ship with reone.",
                        true);
        }
    } else if (options.antialiasing == graphics::AntiAliasing::Fsr) {
        ImGui::TextColored(ImVec4(0.55f, 0.7f, 0.85f, 1.0f),
                           "FidelityFX Super Resolution 2.2.1   temporal, vendor-neutral");
    }

    // DLSS names its render scale rather than taking a number, so under DLSS
    // the mode IS the scale and the slider below reads it back rather than
    // setting it. Two controls free to disagree about one ratio would be a bug
    // waiting to happen.
    static const char *kDlssModeNames[] = {"DLAA (1.00x)", "Quality (0.67x)", "Balanced (0.58x)",
                                           "Performance (0.50x)", "Ultra Performance (0.33x)"};
    static_assert(IM_ARRAYSIZE(kDlssModeNames) ==
                      static_cast<int>(graphics::DlssMode::UltraPerformance) + 1,
                  "every DlssMode needs a name: this array is indexed by the enum");
    if (dlssSelected) {
        ImGui::BeginDisabled(!dlssAvailable);
        int dlssMode = static_cast<int>(staged.dlssMode);
        if (ImGui::Combo("DLSS mode", &dlssMode, kDlssModeNames, IM_ARRAYSIZE(kDlssModeNames))) {
            staged.dlssMode = static_cast<graphics::DlssMode>(dlssMode);
        }
        settingHint("The ratio DLSS renders at before reconstructing to display resolution. "
                    "DLAA is native - no upscaling, anti-aliasing and denoising only. "
                    "It changes target sizes, so Apply is required.",
                    true);
        ImGui::EndDisabled();
    }

    // FSR takes a free ratio; DLSS takes it from the mode above. Disabled
    // rather than hidden under DLSS so the number it resolved to stays visible.
    ImGui::BeginDisabled(staged.antialiasing != graphics::AntiAliasing::Fsr);
    float shownScale = staged.antialiasing == graphics::AntiAliasing::DlssRr
                           ? graphics::dlssModeScale(staged.dlssMode)
                           : staged.renderScale;
    if (ImGui::SliderFloat("Render scale", &shownScale, 0.25f, 1.0f, "%.3f")) {
        staged.renderScale = shownScale;
    }
    settingHint("Raster and trace at this fraction of display resolution; the upscaler "
                "reconstructs the display image. Under DLSS the mode above owns it. "
                "It changes target sizes, so Apply is required.", true);
    ImGui::EndDisabled();

    // One dial, three implementations, because only one of them can run: RCAS
    // inside FSR, DLSS-RR's own, or the postprocess unsharp mask when neither
    // upscaler is in the slot. Never disabled - there is always something for
    // it to drive.
    ImGui::SliderFloat("Sharpness", &options.sharpness, 0.0f, 1.0f, "%.2f");
    settingHint("0 skips sharpening entirely.\n\n"
                "Under FSR this is RCAS, from inside the upscaler. Everywhere else - DLSS "
                "included - it is an unsharp mask over display colour, the last pass of the "
                "frame.\n\n"
                "DLSS is on the mask side despite exposing a sharpness field of its own: "
                "that field is inert. Measured here, 0.0 against 1.0 moved the mean image "
                "gradient by 0.004%%. NVIDIA removed sharpening from DLSS in 3.1 and Ray "
                "Reconstruction kept the field without the deprecation marker.\n\n"
                "RCAS and an unsharp mask do not agree numerically, so expect apparent "
                "sharpness to shift when you change the slot. What it cannot do any more is "
                "sharpen one image twice, which two separate dials made easy.");

    // Denoising, here rather than on the Path tracing tab because it is now a
    // choice between two things in the same slot: NRD denoises the traced
    // channels before the resolve, and DLSS-RR does the denoising itself and
    // leaves nothing for NRD to do. Keeping them a tab apart hid that they are
    // alternatives.
#ifdef R_ENABLE_NRD
    ImGui::SeparatorText("Denoising");
    const bool tracing = options.mode == graphics::RenderMode::PathTracing;
    const bool dlssResolving = options.antialiasing == graphics::AntiAliasing::DlssRr &&
                               _engine._graphicsModule->renderer().rayReconstructionAvailable();
    if (!tracing) {
        ImGui::TextDisabled("Path tracing only - the raster modes have no noise to denoise.");
    } else if (dlssResolving) {
        ImGui::TextDisabled("DLSS Ray Reconstruction is denoising; NRD does not run.");
    }
    ImGui::BeginDisabled(!tracing || dlssResolving);
    ImGui::Checkbox("NRD denoiser", &options.ptDenoise);
    settingHint("Diffuse and specular, through RELAX. Off shows the raw traced frame; "
                "debug views always bypass it. Its tuning is under Advanced.");
    // Order matches the enum, so the index is the value.
    static const char *kShadowFilterNames[] = {"Off", "Denoiser"};
    static_assert(IM_ARRAYSIZE(kShadowFilterNames) ==
                      static_cast<int>(graphics::ShadowFilter::Denoiser) + 1,
                  "every ShadowFilter needs a name: this array is indexed by the enum");
    int shadowFilter = static_cast<int>(options.ptShadowFilter);
    if (ImGui::Combo("Direct channel", &shadowFilter, kShadowFilterNames,
                     IM_ARRAYSIZE(kShadowFilterNames))) {
        options.ptShadowFilter = static_cast<graphics::ShadowFilter>(shadowFilter);
    }
    settingHint("What settles primary-vertex direct light. Two architectures, not two "
                "strengths.\n\n"
                "Off leaves it to the temporal resolve. Default, and worth knowing why: blue noise "
                "puts its error at high spatial frequency, which is what a temporal resolve "
                "averages away and what FSR's clamp rejects. Measured over 32 FSR frames, blurring "
                "made the picture less stable, not more - which is why the geometry-sized "
                "penumbra blur that used to sit here was removed rather than defaulted off.\n\n"
                "Denoiser gives the channel its own NRD instance, which estimates variance per "
                "pixel and sizes its kernel from that - so a clean region keeps its detail. Not "
                "the same as turning the direct channel off, which sums this signal into the "
                "diffuse one: there it gets a kernel chosen for the bounce noise, because the two "
                "then share a single variance estimate dominated by the wrong term.");
    if (ImGui::TreeNode("Direct denoiser tuning")) {
        ImGui::SliderFloat("History", &options.ptNrdDirectAccumulationTime, 0.0f, 2.0f, "%.2f s");
        settingHint("Deliberately shorter than the bounce channel's. A shadow edge moves with "
                    "whatever casts it, so history that suits slow indirect light is lag here.");
        ImGui::SliderInt("A-trous iterations", &options.ptNrdDirectAtrousIterations, 2, 8);
        settingHint("Each iteration doubles the kernel's reach. Few, because this signal arrives "
                    "converged outside the penumbra and the extra width is spent on detail.");
        ImGui::SliderFloat("Luminance phi", &options.ptNrdDirectPhiLuminance, 0.0f, 16.0f, "%.2f");
        settingHint("The edge-stopping term, divided by the estimated variance. Low keeps edges by "
                    "rejecting any tap that differs; high lets the filter average across them. "
                    "This is the dial that decides whether a converged penumbra gradient survives.");
        ImGui::TreePop();
    }
    ImGui::Checkbox("Direct light at resolve", &options.ptDirectChannel);
    settingHint("Direct light on the pixels you are looking at skips the denoiser and is applied "
                "at the resolve. A denoiser is built for indirect light - smooth, slow, worth a "
                "wide kernel - and the same kernel softens the contact edge of a shadow. This "
                "channel is stratified where it is sampled instead, and the temporal resolve takes "
                "what noise is left. Off routes it back through the denoiser.");
    if (ImGui::TreeNode("Denoiser tuning")) {
        ImGui::SeparatorText("Temporal accumulation");
        ImGui::SliderFloat("History", &options.ptNrdAccumulationTime, 0.0f, 2.0f, "%.2f s");
        settingHint("How long light is remembered, in seconds rather than frames. NRD converts it "
                    "against the measured frame rate, which is what it asks for: a fixed frame "
                    "count is a shorter and shorter window as the frame rate rises, and the "
                    "spatial filter widens to cover what the history stops carrying.");
        ImGui::SliderFloat("Responsive history", &options.ptNrdFastAccumulationTime, 0.0f, 1.0f, "%.2f s");
        settingHint("The short history the long one is clamped against. Shorter reacts faster to "
                    "lighting changes and keeps more noise.");
        ImGui::SliderInt("History fix frames", &options.ptNrdHistoryFixFrames, 0, 8);
        ImGui::SeparatorText("Spatial filtering (pixels)");
        ImGui::SliderFloat("Diffuse prepass radius", &options.ptNrdDiffusePrepassBlurRadius, 0.0f, 60.0f, "%.0f");
        ImGui::SliderFloat("Specular prepass radius", &options.ptNrdSpecularPrepassBlurRadius, 0.0f, 60.0f, "%.0f");
        settingHint("Spatial reuse before accumulation. Not optional here: the tracer picks one "
                    "lobe per pixel, so the pixels that went diffuse carry no specular distance at "
                    "all, and NRD asks for a real prepass whenever the sampling is probabilistic.");
        ImGui::SeparatorText("History rejection");
        static constexpr const char *kRejectionHint =
            "Larger = more tolerant. This is the pair that decides whether a neighbour or a history "
            "sample belongs to the same surface, so opening them up hides noise by reusing across "
            "normals and depths that do not match - which is contact shadows and sharp folds gone. "
            "NRD's own value for both is 0.15.";
        ImGui::SliderFloat("Lobe angle fraction", &options.ptNrdLobeAngleFraction, 0.01f, 1.0f, "%.2f");
        settingHint(kRejectionHint);
        ImGui::SliderFloat("Roughness fraction", &options.ptNrdRoughnessFraction, 0.01f, 1.0f, "%.2f");
        settingHint(kRejectionHint);
        ImGui::SliderFloat("Disocclusion threshold", &options.ptNrdDisocclusionThreshold, 0.001f, 0.2f, "%.3f",
                           ImGuiSliderFlags_Logarithmic);
        settingHint(kRejectionHint);
        ImGui::Checkbox("Anti-firefly", &options.ptNrdAntiFirefly);


        ImGui::SeparatorText("RELAX");
        ImGui::SliderInt("A-trous iterations", &options.ptNrdAtrousIterations, 2, 8);
        settingHint("Wavelet passes. Each doubles the reach of the filter while its edge stoppers "
                    "keep it off the edges - which is how RELAX covers ground without the flat "
                    "blur.", true);
        ImGui::SliderFloat("Diffuse luminance phi", &options.ptNrdDiffusePhiLuminance, 0.0f, 8.0f, "%.2f");
        ImGui::SliderFloat("Specular luminance phi", &options.ptNrdSpecularPhiLuminance, 0.0f, 8.0f, "%.2f");
        settingHint("Luminance edge stoppers. Smaller keeps more detail and more noise with it.", true);
        ImGui::SliderFloat("Depth threshold", &options.ptNrdDepthThreshold, 0.0f, 0.05f, "%.4f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Specular lobe slack", &options.ptNrdSpecularLobeAngleSlack, 0.0f, 2.0f, "%.2f deg");
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
#endif

    // A creative grade is display policy, not an all-mode feature toggle:
    // Retro carries a finished reference picture through the linear chain and
    // must not reinterpret it as radiance.
    ImGui::SeparatorText("Display");
    ImGui::Checkbox("Grade", &options.grade);
    settingHint("Exposure and the tone curve. The display transform itself always runs; off is the "
                "ungraded diagnostic. Retro is never graded - its colour is the original's, not "
                "radiance.");
    static constexpr const char *kTonemapNames[] = {"Off (linear)", "Gran Turismo"};
    ImGui::Combo("Tonemap", &options.tonemap, kTonemapNames, 2);
    settingHint("The raster resolves already write display colour, so the transform passes them "
                "through untouched.");
    ImGui::SliderFloat("Exposure", &options.exposure, 0.05f, 8.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    settingHint("The raster resolves already write display colour, so the transform passes them "
                "through untouched.");

}

void Editor::graphicsQualityTab() {
    auto &options = _engine._options.graphics;

    // Both belong to the PBR resolve: occlusion is a term inside it and
    // reflections are a dispatch over the image it produced. Retro is the
    // original's model, which had neither, so the pair sit disabled there
    // rather than quietly doing nothing.
    ImGui::BeginDisabled(options.mode != graphics::RenderMode::PBR);
    ImGui::Checkbox("SSAO", &options.ssao);
    settingHint("Folded into the PBR resolve, where the depth and normals it needs are already "
                "read. Off, the branch is not taken.",
                true);
    ImGui::Checkbox("SSR", &options.ssr);
    settingHint("A second dispatch over the resolved image, ahead of anti-aliasing and "
                "transparency. Off, the pass is not recorded at all.",
                true);
    ImGui::EndDisabled();
    ImGui::SliderInt("Max lights", &options.maxLights, 1, graphics::kMaxLights);
    settingHint("Lights a frame may carry, out of the slots the uniform block is sized for. Below "
                "the ceiling on purpose: the raster resolves walk this loop per pixel and the "
                "tracer walks its selection table per path vertex, so the cost is in how many are "
                "admitted. The ceiling only costs uniform bytes.");
    if (ImGui::TreeNode("Shadow budgets (not yet used)")) {
        ImGui::TextDisabled("Nothing reads these. Recorded ahead of the pass that will.");
        ImGui::SliderInt("Directional shadows", &options.maxDirectionalShadows, 0, 4);
        settingHint("Cascaded directional maps a frame may hold.");
        ImGui::SliderInt("Point shadows", &options.maxPointShadows, 0, 32);
        settingHint("Cube maps a frame may hold.");
        ImGui::TreePop();
    }
    // One section for both grades, because the pair is only legible together.
    // The same sources are graded twice against different transport - the bake
    // IS the indirect light in PBR while the tracer computes that transport and
    // retires the bake toward zero - and that disagreement reads as a
    // disagreement only when the two numbers are in view at once. They lived a
    // tab apart, which made every comparison a tab switch and left whichever
    // group was greyed looking like the only one there was.
    //
    // Same scale on both sides, logarithmic to 32: a value moved from one
    // subsection to the other has to mean the same distance on the dial, and
    // the defaults sit at 1-2.5, so a linear ceiling of 16 spent most of its
    // travel above anything ever set.
    ImGui::SeparatorText("Lighting");
    static constexpr float kLightIntensityMax = 32.0f;
    static constexpr ImGuiSliderFlags kLightIntensityFlags = ImGuiSliderFlags_Logarithmic;
    const bool pbrRunning = options.mode == graphics::RenderMode::PBR;
    const bool tracerRunning = options.mode == graphics::RenderMode::PathTracing;

    ImGui::TextDisabled("PBR%s", pbrRunning ? " (running)" : "");
    ImGui::Indent();
    ImGui::PushID("pbr-lighting");
    ImGui::BeginDisabled(!pbrRunning);
    ImGui::SliderFloat("Lightmap", &options.pbrLightmapIntensity, 0.0f, kLightIntensityMax, "%.2f",
                       kLightIntensityFlags);
    settingHint("Strength of the area's baked irradiance, which is this mode's indirect light, so "
                "it belongs at full strength. The tracer has its own dial for the same bake - "
                "Lightmap cache, below - kept near zero because it computes that transport for "
                "real and would otherwise count it twice. That pair is the one number the two "
                "modes are meant to disagree on.",
                true);
    ImGui::SliderFloat("Sky", &options.pbrSkyIntensity, 0.0f, kLightIntensityMax, "%.2f",
                       kLightIntensityFlags);
    settingHint("This mode's sky light. Path tracing has its own below: the two grades are "
                "authored against different transport, so one number for both made every "
                "adjustment a question of which mode was running.",
                true);
    ImGui::SliderFloat("Emissive", &options.pbrEmissiveIntensity, 0.0f, kLightIntensityMax, "%.2f",
                       kLightIntensityFlags);
    settingHint("Lamps, screens and glowing panels, in this mode.", true);
    ImGui::SliderFloat("Direct light", &options.pbrDirectIntensity, 0.0f, kLightIntensityMax, "%.2f",
                       kLightIntensityFlags);
    settingHint("Point and spot lights, in this mode.", true);
    ImGui::SliderFloat("Sun", &options.pbrSunIntensity, 0.0f, kLightIntensityMax, "%.2f",
                       kLightIntensityFlags);
    settingHint("Directional lights, in this mode.", true);
    ImGui::EndDisabled();
    ImGui::PopID();
    ImGui::Unindent();

    // Identical labels on both sides - the sources are the same sources - so
    // the two groups need their own ID scopes or ImGui gives "Sky" one widget
    // state shared between them.
    ImGui::TextDisabled("Path tracing%s", tracerRunning ? " (running)" : "");
    ImGui::Indent();
    ImGui::PushID("pt-lighting");
    ImGui::BeginDisabled(!tracerRunning);
    ImGui::SliderFloat("Lightmap cache", &options.ptLightmapIntensity, 0.0f, kLightIntensityMax,
                       "%.2f", kLightIntensityFlags);
    settingHint("Near zero on purpose: the tracer computes that transport for real, so adding the "
                "bake on top counts it twice. PBR keeps its own strength for the same bake above - "
                "that pair is the one number the two modes are meant to disagree on.",
                true);
    ImGui::SliderFloat("Sky", &options.ptSkyIntensity, 0.0f, kLightIntensityMax, "%.2f",
                       kLightIntensityFlags);
    settingHint("This mode's sky light, graded against transport it computes rather than a bake.",
                true);
    ImGui::SliderFloat("Backdrop", &options.ptBackdropIntensity, 0.0f, kLightIntensityMax, "%.2f",
                       kLightIntensityFlags);
    settingHint("Painted distance: Taris' cityscape, Manaan's towers. Its own scale beside the "
                "sky's, and deliberately not the emissive one. Emissive grades lamps, screens and "
                "panels - objects standing in the scene that also light it. A backdrop is a "
                "picture of a distance nobody modelled: it ends the path exactly as the sky does, "
                "and there is nothing behind it to receive what it might emit. Sharing a dial "
                "meant choosing between the skyline reading right and the interiors reading "
                "right. 1.0 is the texture as authored.",
                true);
    ImGui::SliderFloat("Emissive", &options.ptEmissiveIntensity, 0.0f, kLightIntensityMax, "%.2f",
                       kLightIntensityFlags);
    settingHint("Lamps, screens and glowing panels, in this mode.", true);
    ImGui::SliderFloat("Direct light", &options.ptDirectIntensity, 0.0f, kLightIntensityMax, "%.2f",
                       kLightIntensityFlags);
    settingHint("Point and spot lights, in this mode.", true);
    ImGui::SliderFloat("Sun", &options.ptSunIntensity, 0.0f, kLightIntensityMax, "%.2f",
                       kLightIntensityFlags);
    settingHint("Directional lights, in this mode.", true);
    ImGui::SliderFloat("Emissive gamma", &options.emissiveGamma, 0.1f, 4.0f, "%.2f");
    settingHint("Exponent authored radiance is decoded with: emission, sky and backdrop imagery. "
                "Only the tracer reads it, which is why it sits in this group. Separate from "
                "Albedo gamma under Materials, and it should stay at 2.2. That one is a look "
                "control - Odyssey art was authored to be multiplied by light unconverted, so any "
                "decode is a compromise. This one is not a compromise: an authored glow colour is "
                "the colour emitted, and 2.2 is just the encoding it was stored in. They used to "
                "be one number, so grading a room's reflectance also changed how bright its lamps "
                "and its skyline were.",
                true);
    ImGui::EndDisabled();
    ImGui::PopID();
    ImGui::Unindent();
    ImGui::Checkbox("Fog", &options.fog);
    settingHint("The area's authored fog, resolved once for every mode from the depth buffer. "
                "Exponential in height above the walkmesh rather than a flat ramp in distance, "
                "so the horizon fogs out while the sky survives.");
    ImGui::BeginDisabled(!options.fog);
    ImGui::SliderFloat("Fog gradient", &options.fogHeight, 0.5f, 512.0f, "%.0f m",
                       ImGuiSliderFlags_Logarithmic);
    settingHint("Altitude above the walkmesh at which the fog has thinned to a hundredth of its "
                "ground density - which is to say, what kind of fog this is. A few units is a "
                "ground layer: distant hills veiled, the sky above the layer clear. Tens of units "
                "fills the volume and becomes atmospheric haze, where the sky fogs with everything "
                "else. Density is not a dial; it follows the ramp the area authored.",
                true);
    ImGui::EndDisabled();
    ImGui::Checkbox("Grass", &options.grass);
    ImGui::Checkbox("Particles", &options.particles);
    settingHint("Admit emitter particles to the scene, or leave them out of it entirely. Off is "
                "not a draw-time skip: the emitters are never collected, so their instances cost "
                "nothing in the scene, the acceleration structure or the frame.");
    ImGui::Checkbox("Transparency", &options.transparency);
    settingHint("Draw the forward transparency pass, or stop at the opaque image.\n\n"
                "One pass carries everything non-opaque - blended and additive geometry, sabers, "
                "particles, the transparent halves of models - so this removes all of it at once. "
                "That is what makes it a diagnostic rather than a quality dial: an artefact that "
                "survives with this off is not transparency's, and one that vanishes is.\n\n"
                "It differs from Particles above, which decides whether emitters reach the scene "
                "at all; this skips the pass they would have been drawn in.");
    // Wired straight: density is a GPU gate over budgets baked at the slider
    // maximum (kGrassDensityCap), so dragging costs a push-constant change.
    // The old committed-on-release dance existed to avoid re-materialising
    // every cluster per mouse-move; that rebuild no longer exists.
    ImGui::SliderFloat("Thin transmission", &options.thinTransmission, 0.0f, 1.0f, "%.2f");
    settingHint("Light a surface with no thickness passes to its far side. Leaves, cloth and grass "
                "are alpha cutouts standing in for exactly that, so they are lit from both sides "
                "and shadowed from whichever side the light is on. Zero shades them as solids, "
                "which sends every back-facing leaf black.");
    ImGui::SliderFloat("Grass density", &options.grassDensity, 0.0f, 64.0f, "%.2fx",
                       ImGuiSliderFlags_Logarithmic);
    settingHint("Multiplies the area's authored density, so areas keep their relative variation. "
                "Live: the dial gates the active cluster prefix on the GPU.");
    if (ImGui::TreeNode("Grass shape")) {
        const char *cardShapes[] {"Quad", "AABB", "OBB", "K-gon", "Grid"};
        int cardShape = static_cast<int>(options.grassCardShape);
        if (ImGui::Combo("Card outline", &cardShape, cardShapes, IM_ARRAYSIZE(cardShapes)))
            options.grassCardShape = static_cast<graphics::GrassCardShape>(cardShape);
        ImGui::SliderInt("Card sides", &options.grassCardSides, 3, 16);
        ImGui::SliderInt("Card grid", &options.grassCardGrid, 2, 32);
        ImGui::SliderFloat("Card aspect", &options.grassCardAspect, 0.1f, 4.0f, "%.2f");
        settingHint("The fitted outline the blade texture is cut to, and the card's own "
                    "width-over-length aspect.");
        ImGui::ColorEdit3("Colour", &options.grassColor.x);
        settingHint("Multiplies the area's authored blade texture.");
        ImGui::SliderFloat("Draw radius", &options.grassRadius, 0.0f, 256.0f, "%.0f");
        settingHint("Blades past this are dropped outright rather than faded, so it reads as a "
                    "hard edge if you set it inside the ground the camera can see.");
        ImGui::SliderFloat("Length", &options.grassLength, 0.0f, 8.0f, "%.2fx");
        settingHint("Multiplies the area's authored quad size rather than setting a world length, "
                    "so an area that authored short grass keeps it short.");
        ImGui::SliderFloat("Length variance", &options.grassLengthVariance, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Width", &options.grassWidth, 0.0f, 0.5f, "%.3f");
        settingHint("As a fraction of length. Thin blades are subpixel at distance, which is where "
                    "a temporal resolver starts to shimmer - widening costs less than it looks.");
        ImGui::SliderFloat("Wind strength", &options.grassWindStrength, 0.0f, 2.0f, "%.2f rad");
        settingHint("Extra bend at full gust; zero is still air. The wind rotates each blade's arc "
                    "downwind rather than displacing its tip, so a blade keeps the length it was "
                    "authored with however hard it is blowing.");
        ImGui::SliderFloat("Wind direction", &options.grassWindDirection, -3.1416f, 3.1416f, "%.2f rad");
        ImGui::SliderFloat("Wind speed", &options.grassWindSpeed, 0.0f, 10.0f, "%.2f");
        ImGui::SliderFloat("Wind wavelength", &options.grassWindWavelength, 0.1f, 64.0f, "%.1f");
        settingHint("Distance between crests. This is what you watch cross a field, so it wants to "
                    "be several blades wide - shorter than that and the field boils instead.");
        ImGui::SliderFloat("Wind gust", &options.grassWindGust, 0.0f, 1.0f, "%.2f");
        settingHint("Share of the strength carried by the slow, long wave rather than the fast "
                    "short one. At zero the grass rustles in place; at one it heaves.");
        ImGui::SliderFloat("Orientation", &options.grassOrientation, -3.1416f, 3.1416f, "%.2f rad");
        ImGui::SliderFloat("Orientation variance", &options.grassOrientationVariance, 0.0f, 6.2832f,
                           "%.2f rad");
        settingHint("A full turn of variance is the random scatter a field wants. Zero points every "
                    "blade the same way - a wind direction, or a lawn that has been mown - and the "
                    "orientation above is the way they point.");
        ImGui::SliderFloat("Curvature", &options.grassCurvature, -2.0f, 2.0f, "%.2f rad");
        settingHint("Total bend of the blade's arc. The spine is a true arc, so a bent blade keeps "
                    "the length a straight one has.");
        ImGui::SliderFloat("Curvature variance", &options.grassCurvatureVariance, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Sparsity", &options.grassSparsity, 0.0f, 0.99f, "%.2f");
        settingHint("Fraction of slots left empty. Density sets the grid; this buys the clumping "
                    "that keeps a field from looking planted.");
        ImGui::SliderFloat("Displacement", &options.grassDisplacement, 0.0f, 3.0f, "%.2f cells");
        settingHint("How far a blade may wander from its slot. Above one they cross into "
                    "neighbouring cells, which breaks up the grid at the cost of wider bounds.");
        ImGui::SliderFloat("Root offset", &options.grassYOffset, -0.5f, 0.5f, "%.3f");
        settingHint("Sinks the root below the ground, as a fraction of length, so blades do not "
                    "appear to float on uneven terrain.");
        ImGui::SliderInt("Blades per cluster", &options.grassBladesPerCluster, 1, 32);
        settingHint("The area authored its clusters for cardboard, where one cluster is a card "
                    "whose texture already draws a tuft. One strand per cluster replaces that tuft "
                    "with a single blade, which is why a field reads as stubble. Raise the "
                    "triangle budget alongside this, or the ceiling divides by it and you get the "
                    "same blades gathered into clumps rather than more of them.");
        ImGui::SliderFloat("Roughness", &options.grassRoughness, 0.0f, 1.0f, "%.2f");
        settingHint("Set outright, because a strand has no texture for roughness to be derived "
                    "from. Reaches path tracing only - the raster resolve takes roughness from the "
                    "G-buffer and does not read it.");
        ImGui::SliderInt("Triangle budget", &options.grassTriangleBudget, 0,
                         graphics::kMaxGrassTriangleBudget);
        settingHint("A hard cap at nine triangles a blade, scaling the module's blades down "
                    "uniformly. It does not concentrate them near you: blades past the draw radius "
                    "still hold their share, so a dense near field means raising this until the "
                    "near field looks right and spending most of it out of sight. Zero disables "
                    "the cap.");
        ImGui::TreePop();
    }
    ImGui::SliderFloat("Draw distance", &options.drawDistance, 1.0f, 1000.0f, "%.0f");

    graphicsReapplySection();

    ImGui::SeparatorText("Requires restart");
    int textureQuality = static_cast<int>(options.textureQuality);
    int anisotropic = options.anisotropicFiltering;
    static constexpr const char *kRestartHint =
        "Read once, before anything a rebuild could reach: the texture pack is chosen while the "
        "game directory is opened, and anisotropy is baked into each texture's sampler as it "
        "loads.";
    ImGui::BeginDisabled();
    // Named, not numbered. Both of these are stored as indices whose meaning is
    // the opposite of how a slider reads: TextureQuality::High is 0, so the
    // slider sat at its minimum while the setting was its best, and anisotropy
    // is an exponent - exp2 of it, in resource/provider/textures.cpp - so a 4
    // meant 16x. Against the launcher, which spells both out, the engine looked
    // like it was disagreeing when it was only being read wrong.
    static const char *kTextureQualityNames[] = {"High", "Medium", "Low"};
    ImGui::Combo("Texture quality", &textureQuality, kTextureQualityNames,
                 IM_ARRAYSIZE(kTextureQualityNames));
    settingHint(kRestartHint, true);
    static const char *kAnisotropyNames[] = {"Off", "2x", "4x", "8x", "16x"};
    ImGui::Combo("Anisotropy", &anisotropic, kAnisotropyNames, IM_ARRAYSIZE(kAnisotropyNames));
    settingHint("Stored as an exponent: the sampler is built at two to this power, so 4 is 16x. "
                "The launcher's list is the same one.\n\n"
                "Read once, before anything a rebuild could reach: the texture pack is chosen "
                "while the game directory is opened, and anisotropy is baked into each texture's "
                "sampler as it loads.", true);
    ImGui::EndDisabled();
}


void Editor::graphicsPathTracingTab() {
    auto &options = _engine._options.graphics;
    auto &staged = _engine.stagedGraphicsOptions();

    if (options.mode != graphics::RenderMode::PathTracing) {
        ImGui::TextDisabled("Inactive - run with --mode path-tracing.");
        settingHint("Settings still save and apply when it is.");
    }
    // Everything here rides in push constants, so a change applies on the next
    // frame with nothing rebuilt. The editor remains the place to tune beside
    // the picture; config and command-line values make a calibration repeatable.
    if (ImGui::SliderInt("Samples per pixel", &options.pathTracingSamples, 1, 64)) {
        options.pathTracingSamples = std::max(1, options.pathTracingSamples);
    }
    settingHint("Cost is near linear; noise falls as sqrt.");
    if (ImGui::SliderInt("Bounces", &options.ptBounces, graphics::kMinPtBounces,
                         graphics::kMaxPtBounces)) {
        options.ptBounces = std::clamp(options.ptBounces, graphics::kMinPtBounces,
                                       graphics::kMaxPtBounces);
    }
    settingHint("Path depth after the primary hit. Deeper paths carry light around corners; the "
                "lightmap cache already answers much of it on static geometry.\n\n"
                "Zero is a real setting, not a disabled tracer: the primary vertex still draws "
                "next-event estimation, so the frame is direct lighting with no indirect at all. "
                "That is the reference the indirect terms are judged against.");

    ImGui::Checkbox("Next-event estimation", &options.ptNee);
    settingHint("Draw a light at each shading vertex and trace a shadow ray at it.\n\n"
                "This is how the tracer finds analytic lights at all - they are not geometry, so "
                "a BSDF ray cannot hit them. Off, the direct term goes to nothing and what remains "
                "is emissive surfaces and sky, which is exactly what makes it a diagnostic: it "
                "separates what NEE contributes from what the path finds on its own. Leave it on "
                "for any picture you intend to judge.");
    ImGui::BeginDisabled(!options.ptNee);
    if (ImGui::SliderInt("NEE light samples", &options.ptNeeSamples, graphics::kMinPtNeeSamples,
                         graphics::kMaxPtNeeSamples)) {
        options.ptNeeSamples = std::clamp(options.ptNeeSamples, graphics::kMinPtNeeSamples,
                                          graphics::kMaxPtNeeSamples);
    }
    settingHint("Light samples per shading vertex. Direct-light noise falls as 1/sqrt of this for "
                "a linear cost in shadow rays.\n\n"
                "Cheaper than the same factor in samples per pixel, which re-traces the primary "
                "hit as well - so a frame whose noise is mostly shadow noise is better served "
                "here, and one that is noisy in the bounces is not.");
    ImGui::EndDisabled();

    // The intensity dials are on the Quality tab, in one Lighting section
    // beside PBR's: the two grades are the same sources graded against
    // different transport, and a tab between them made every comparison a tab
    // switch. What stays here is what only the tracer has - the geometry it
    // samples emitters with, below.

    ImGui::SeparatorText("Ray setup");
    ImGui::SliderFloat("Origin offset", &options.ptRayOffset, 0.0001f, 0.1f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    settingHint("Too small: acne and black speckling.\nToo large: light leaks at contact edges.");
    ImGui::SliderFloat("Emitter radius", &options.ptPointEmitterRatio, 0.01f, 0.5f, "%.2f x radius");
    settingHint("Point lights are spheres sized as a fraction of their influence radius, so falloff "
                "and penumbra are one quantity: the solid angle the emitter subtends.\n\n"
                "Brightness-neutral - this grades how soft shadows are and how hot a surface gets "
                "against a lamp, not the overall level.");
    ImGui::SliderFloat("Sun angular size", &options.ptSunAngularSize, 0.05f, 10.0f, "%.2f deg");
    settingHint("Ctrl+click to type a value. The sun is not at a physical distance, so it keeps an "
                "authored angle. Here rather than with the intensities: it is emitter geometry, "
                "like Emitter radius above - it decides how soft the sun's shadow is, not how "
                "bright the sun is.");
}

void Editor::graphicsAdvancedTab() {
    graphicsDebugViewSection();

    auto &options = _engine._options.graphics;

    ImGui::SeparatorText("Scene capture");
    if (ImGui::Button("Capture scene state")) {
        requestSceneCapture();
    }
    settingHint("Writes everything needed to identify what is on screen to a numbered folder "
                "beside the executable: the frame, every pipeline target as .npy including the "
                "triangle-id image, one image per debug channel, the object list with names and "
                "classifications, the device-side object and material records, and the camera and "
                "module. A triangle id read out of a pixel falls in exactly one record's range, "
                "and that record names a material and an object - which is what turns 'that thing "
                "over there' into a name.");
    if (!_lastCapturePath.empty()) {
        ImGui::TextDisabled("%s", _lastCapturePath.c_str());
    }

    // The three dials that decide how the tracer trades speckle against
    // sharpness. Here rather than buried in a constant because the right
    // value is content-dependent and only the picture decides it.
    ImGui::SeparatorText("Variance and gloss");
    ImGui::SliderFloat("Bounce roughness", &options.ptBounceRoughness, 0.0f, 1.0f, "%.2f");
    settingHint("Path regularisation: the roughness a surface is treated as having after the first "
                "scatter. A tight lobe reached through a bounce is a caustic, and a caustic at one "
                "sample per pixel is a firefly. Raise for less speckle and duller indirect "
                "reflections; 0 disables it.");
    ImGui::SliderFloat("Roughness floor", &options.ptRoughnessFloor, 0.0f, 1.0f, "%.2f");
    settingHint("The lowest roughness any surface may take. Odyssey has no roughness channel - "
                "diffuse alpha stands in - so this is what keeps an authored mirror from being a "
                "perfect one, and why a roughness scale of zero does not give you a mirror. The "
                "PBR resolve clamps against this too.");
    ImGui::SliderFloat("Indirect clamp", &options.ptIndirectClamp, 0.0f, 16.0f, "%.2f");
    settingHint("Ceiling on a single indirect sample; 0 is off. The blunt one: it truncates energy "
                "rather than widening a lobe, so it darkens whatever it fixes. For the cases "
                "regularisation alone will not settle.");


    ImGui::SeparatorText("Diagnostics");
    ImGui::Checkbox("Trace stats", &options.ptTraceStats);
    settingHint("GPU counters in the engine log. Costs frame time; leave off when measuring.");
}

/**
 * Runs across frames, not within one.
 *
 * The debug channels are the point of the capture, and each is a whole frame
 * rendered a different way - the channel is read at trace and resolve time, not
 * composited afterwards. Asking for them inside a frame does nothing at all:
 * renderFrame refuses to nest, so the first attempt wrote twenty-one identical
 * copies of the same picture and looked like it had worked.
 *
 * So one channel per frame. The image written on any given call is the one the
 * frame that just finished was rendered with.
 */
void Editor::performPendingSceneCapture() {
    if (_captureRequested) {
        _captureRequested = false;
        if (!beginSceneCapture()) {
            return;
        }
    }
    if (_captureStep < 0) {
        return;
    }
    auto &options = _engine._options.graphics;
    const int view = _captureStep;
    std::ostringstream name;
    name << "debug_" << std::setfill('0') << std::setw(2) << view << ".tga";
    try {
        auto shot = _engine._services->graphics.renderer.captureFrame();
        auto stream = FileOutputStream(_captureDir / name.str());
        graphics::TgaWriter(shot).save(stream);
    } catch (const std::exception &e) {
        warn(std::string("Scene capture: debug channel ") + std::to_string(view) +
             " failed: " + e.what());
    }
    if (view >= graphics::kMaxDebugView) {
        options.debugView = _captureRestoreView;
        _captureStep = -1;
        _lastCapturePath = _captureDir.string();
        info("Scene state captured to " + _lastCapturePath);
        return;
    }
    options.debugView = view + 1;
    ++_captureStep;
}

bool Editor::beginSceneCapture() {
    namespace fs = std::filesystem;
    // Beside the executable, as asked. SDL3 owns this string - freeing it
    // corrupts the heap, see TracingPipeline::loadBlueNoise.
    fs::path root = "capture";
    if (const auto *base = SDL_GetBasePath()) {
        root = fs::path(base) / "capture";
    }
    // Numbered, never overwritten: two captures of the same scene taken a
    // moment apart are usually the whole point of taking them.
    int index = 0;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto &entry : fs::directory_iterator(root, ec)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("state_", 0) != 0)
            continue;
        try {
            index = std::max(index, std::stoi(name.substr(6)) + 1);
        } catch (const std::exception &) {
        }
    }
    std::ostringstream stem;
    stem << "state_" << std::setfill('0') << std::setw(4) << index;
    const fs::path dir = root / stem.str();
    fs::create_directories(dir, ec);

    auto &graphs = _engine._services->scene.graphs;
    auto &graph = graphs.get(game::kSceneMain);
    auto &scene = graph.gpuScene();
    auto &options = _engine._options.graphics;

    // Each stage is independent. The images are the part most likely to fail -
    // a headless run has no swapchain to read - and the tables are the part
    // that actually names an object, so one must not take the other down.
    auto stage = [](const char *what, auto &&body) {
        try {
            body();
        } catch (const std::exception &e) {
            warn(std::string("Scene capture: ") + what + " failed: " + e.what());
        }
    };

    // The frame as shown. The debug channels follow one per frame, driven by
    // performPendingSceneCapture - see the note there for why they cannot be
    // taken here.
    const int restoreDebugView = options.debugView;
    stage("frame", [&] {
        auto shot = _engine._services->graphics.renderer.captureFrame();
        auto stream = FileOutputStream(dir / "frame.tga");
        graphics::TgaWriter(shot).save(stream);
    });

    // Every pipeline target, and the tables that give the ids meaning.
    stage("targets", [&] {
        if (auto *pipeline = graph.renderPipeline()) {
            _engine._renderer->flushFrame();
            pipeline->dumpTargets(dir);
            pipeline->dumpSceneRecords(dir);
        }
    });

    stage("objects", [&] {
        // id is the number records.tsv prints as object_id, which is what makes
        // a triangle id sampled out of the G-buffer resolvable to a name.
        // The game object behind each piece of geometry, where there is one.
        //
        // Everything else in this table names geometry - model, node, material
        // - which answers "what am I looking at" but not "what can I do to it".
        // The tag and game id answer the second, and they are what the console
        // takes: selectobjectbytag <tag> then action reaches the same
        // onObjectClick a mouse click does. Without them a capture could name
        // the Ebon Hawk's map console and still leave no way to open it
        // unattended, which is how a renderer defect specific to that screen
        // stayed unreproducible.
        //
        // Keyed on the model scene node each game object owns, which is the
        // same pointer makeObjectEntryView reports as its cull root.
        std::map<const scene::SceneNode *, std::pair<uint32_t, std::string>> byNode;
        if (_engine._game) {
            if (auto module = _engine._game->module()) {
                if (auto area = module->area()) {
                    for (const auto &gameObject : area->objects()) {
                        if (!gameObject) {
                            continue;
                        }
                        if (auto node = gameObject->sceneNode()) {
                            byNode[node.get()] = {gameObject->id(), gameObject->tag()};
                        }
                    }
                }
            }
        }

        std::ofstream out(dir / "objects.tsv");
        out << "id\tkind\tmodel\tnode\tmaterial\tclassification\tclusters\tparticles"
               "\tgame_id\ttag\n";
        for (const auto &object : scene.objects()) {
            const auto view = makeObjectEntryView(graph, scene, object);
            const auto game = byNode.find(static_cast<const scene::SceneNode *>(view.root));
            out << view.id << "\t" << view.kind << "\t" << view.modelName << "\t"
                << view.nodeName << "\t" << view.material << "\t" << view.classification << "\t"
                << view.clusters << "\t" << view.particles << "\t";
            if (game != byNode.end()) {
                out << game->second.first << "\t" << game->second.second << "\n";
            } else {
                // Room geometry, grass, an effect - real geometry with no game
                // object behind it. A dash rather than a blank, so the column
                // reads as answered rather than missing.
                out << "-\t-\n";
            }
        }
    });

    // How to use the folder, written into the folder. A capture read weeks
    // later, or by somebody who did not build it, should not require reading
    // this function to know which file answers which question.
    stage("readme", [&] {
        std::ofstream out(dir / "README.txt");
        out << "Naming what is at pixel (x, y):\n"
               "  1. g_buffer_triangle_id.npy[y][x] -> triangle id\n"
               "  2. records.tsv: the row whose [first_triangle, first_triangle+triangle_count)\n"
               "     contains it -> object_id and material\n"
               "  3. objects.tsv: the row with that id -> model/node, kind, classification,\n     and game_id/tag when a game object owns it\n     -> selectobjectbytag <tag>, then action, drives it from the console\n"
               "  4. materials.tsv: that material -> surface type, feature mask, texture ids\n"
               "\n"
               "Feature mask bits in g_buffer_lightmap alpha (value * 255):\n"
               "  1 envmap, 2 shadows, 4 fog, 8 lightmap, 16 static, 32 thin\n"
               "  thin is set for alpha cutouts, so it doubles as 'this is a cutout'.\n"
               "\n"
               "Surface type in materials.tsv: 0 lit PBR, 1 additive/unlit, 2 unlit radiance\n"
               "  (sky and backdrop imagery - the path ends there).\n"
               "\n"
               "debug_NN.tga is the frame rendered with debug channel NN; 00 is off, so it\n"
               "matches frame.tga. Channel order is the kDebug* numbering in\n"
               "slang/debug_view.slang.\n";
    });

    stage("scene", [&] {
        std::ofstream out(dir / "scene.txt");
        auto module = _engine._game ? _engine._game->module() : nullptr;
        out << "module\t" << (module ? module->name() : "-") << "\n";
        out << "mode\t" << graphics::renderModeName(options.mode) << "\n";
        out << "resolution\t" << options.width << "x" << options.height << "\n";
        if (auto camera = graph.camera()) {
            const auto &m = camera->get().absoluteTransform();
            const glm::vec3 position {m[3]};
            const glm::vec3 forward {-m[2]};
            out << "camera_position\t" << position.x << " " << position.y << " " << position.z
                << "\n";
            out << "camera_forward\t" << forward.x << " " << forward.y << " " << forward.z << "\n";
        }
        out << "objects\t" << scene.objects().size() << "\n";
        out << "counts\t" << scene::formatSceneCounts(scene.counts()) << "\n";
        out << "sky_room\t" << (scene.skyRoom() ? "yes" : "no") << "\n";
        // The dials that change what any of the above means.
        out << "albedo_gamma\t" << options.albedoGamma << "\n";
        out << "sky_intensity\t" << options.ptSkyIntensity << "\n";
        out << "backdrop_intensity\t" << options.ptBackdropIntensity << "\n";
        out << "emissive_intensity\t" << options.ptEmissiveIntensity << "\n";
        out << "max_lights\t" << options.maxLights << "\n";
        out << "debug_view_restored\t" << restoreDebugView << "\n";
    });

    // Hand over to the per-frame sweep: channel 0 renders next frame.
    _captureDir = dir;
    _captureRestoreView = restoreDebugView;
    _captureStep = 0;
    options.debugView = 0;
    return true;
}

void Editor::graphicsDebugViewSection() {
    auto &options = _engine._options.graphics;

    // Heads Advanced rather than holding a tab of its own: it was the only
    // diagnostic control in the window, and a tab with one combo in it is a tab
    // you stop opening. Not a tracer tool either - the channels are G-buffer
    // quantities and every mode draws that G-buffer, so one selection answers
    // in all three.
    ImGui::SeparatorText("Debug overlay");
    ImGui::Checkbox("Bounding boxes and names", &options.debugOverlay);
    settingHint("Draws each object's oriented bounding box and its name over the finished frame, "
                "in every render mode, coloured by kind - rooms blue, creatures red, placeables "
                "green, doors magenta, equipment yellow - with a marker box at every light. Lines "
                "and labels are depth-tested per pixel against the G-buffer: what lies behind "
                "geometry stays visible but goes translucent, so a hidden box still says where "
                "its object is. Not drawn over a debug channel view, which replaces the picture "
                "the boxes would annotate.");
    // Not a GraphicsOptions field: the flag lives behind game/debug.h and is
    // read by the pathfinder itself, so the checkbox mirrors that rather than
    // owning it. Only this one of the four debug toggles is offered - the AABB,
    // walkmesh and trigger flags have no consumer left on this backend, and a
    // control that does nothing is worse than an absent one.
    bool showPath = game::isShowPathEnabled();
    if (ImGui::Checkbox("Pathfinder graph", &showPath)) {
        game::setShowPath(showPath);
    }
    settingHint("Draws the navigation mesh the pathfinder actually walks: each face outlined and "
                "numbered, adjacency in blue and unconnected borders in purple, plus the funnel "
                "of any path being solved. It shares the overlay above - same pass, same depth "
                "image - so a path edge and a bounding box agree about what occludes them.\n\n"
                "Turn it on BEFORE loading a module. The face graph is recorded once as the area "
                "loads and is skipped entirely when this is off, so enabling it in a module "
                "already running shows nothing until something recomputes a path.");
    ImGui::Checkbox("Direct-light parity", &options.parityDirect);
    settingHint("Strips both renderers to the same thing: the shared unoccluded direct diffuse "
                "sum, no shadows, no specular, no bounces, no lightmap, no sky. The two modes "
                "must then produce the same image, so any difference is in their shared inputs "
                "rather than in either assembly. The comparison the one-rendering-path work is "
                "held to.");

    // Order must match the kDebug* numbering in slang/debug_view.slang.
    ImGui::SeparatorText("Debug view");
    static constexpr const char *kDebugViewNames[] = {
        "Off", "Object categories", "Emissive highlight", "Normals",
        "Roughness", "Metallic", "Lightmap", "Albedo",
        "Traced: diffuse radiance", "Traced: specular radiance",
        "Depth", "Traced: noise-free", "Motion", "Material id", "Feature bits",
        "Direct diffuse", "Traced: penumbra",
        "Denoised: diffuse", "Denoised: specular", "Direct: filtered"};
    // Unsized, with the count asserted, because the last three were produced by
    // the resolve and routed by isResolveDebugView for as long as they have
    // existed and were still unreachable here: the list simply stopped at 16,
    // and a combo cannot offer an entry it has no name for. Same shape of
    // omission as the aux-image name tables, and the same guard - adding a
    // channel without naming it is now a build error rather than a channel
    // nobody can select.
    static_assert(std::size(kDebugViewNames) == static_cast<size_t>(graphics::kMaxDebugView) + 1,
                  "every debug channel through kMaxDebugView needs a name here");
    ImGui::Combo("Channel", &options.debugView, kDebugViewNames,
                 static_cast<int>(std::size(kDebugViewNames)));
    settingHint("Replaces the shaded image. Categories: blue rooms, red creatures, green "
                "placeables, magenta doors, yellow equipment, cyan sky. Roughness, metallic and "
                "depth are raw, not shaded. Feature bits: red env-map, green lightmap, blue "
                "static, dimmed when unshadowed.\n\n"
                "The Traced channels are the tracer's output split as it was fed to the denoiser; "
                "the Denoised ones are what came back, still demodulated - before the material is "
                "put back and the components are reassembled - so each shows its own noise rather "
                "than an albedo multiplied over it. Comparing a Traced channel against its "
                "Denoised counterpart is what the pair is for.\n\n"
                "Direct: filtered is neither. Direct light at the primary vertex bypasses NRD "
                "entirely - that is why it exists - and this is that channel after the engine's "
                "own penumbra-sized blur, or the raw channel when the shadow filter is off. "
                "Nothing NRD did shows up here.\n\n"
                "All of them exist only under path tracing; elsewhere they draw a magenta 'not "
                "available' hatch rather than black or some other channel. Any active view skips "
                "anti-aliasing, grade and sharpen - it is not a picture.");
}

void Editor::graphicsMaterialsTab() {
    auto &options = _engine._options.graphics;

    // Governs every material record in both shading modes, so it leads the tab
    // the records belong to rather than sitting under one mode's heading.
    ImGui::SliderFloat("Albedo gamma", &options.albedoGamma, 0.1f, 4.0f, "%.2f");
    settingHint("Exponent authored surface colour is decoded with, in PBR and path tracing alike. "
                "2.2 is sRGB-correct and is the default, but Odyssey content was not authored to "
                "be decoded at all - xoreos, KotOR.js and kvp all multiply the texel by light "
                "unconverted, as the original did. So 2.2 squares an albedo the artist picked "
                "directly (0.5 becomes 0.22) and a path tracer compounds that every bounce; 1.0 "
                "reproduces the reference engines. Reflectance only: light colour, emission and "
                "the output encode are untouched.");
    ImGui::Separator();

    // These edit the shared material records, read by both PBR and the tracer
    // - see applyCategoryOverride in scene/render/admission.cpp.
    static constexpr const char *kCategoryNames[] = {
        "GUI", "Rooms", "Creatures", "Placeables", "Doors",
        "Equipment", "Projectiles", "Cameras", "Uncategorized"};
    static constexpr const char *kCategoryHint =
        "Applied to every surface of the category as its material record is built. Color weight 1 "
        "flat-paints for bug isolation.";
    for (int i = 0; i < 9; ++i) {
        // GUI, projectile and camera models never reach the TLAS.
        if (i == 0 || i == 6 || i == 7) {
            continue;
        }
        auto &override = options.categoryOverrides[i];
        // Opened separately from the hint so the hint hangs off the header,
        // which is the row that is there whether the node is open or shut.
        const bool open = ImGui::TreeNode(kCategoryNames[i]);
        settingHint(kCategoryHint);
        if (open) {
            ImGui::ColorEdit3("Color", override.color);
            ImGui::SliderFloat("Color weight", &override.colorWeight, 0.0f, 1.0f, "%.2f");
            bool overrideRoughness = override.roughness >= 0.0f;
            if (ImGui::Checkbox("Override roughness", &overrideRoughness)) {
                override.roughness = overrideRoughness ? 0.5f : -1.0f;
            }
            if (overrideRoughness) {
                ImGui::SliderFloat("Roughness", &override.roughness, 0.0f, 1.0f, "%.2f");
            }
            ImGui::SliderFloat("Roughness scale", &override.roughnessScale, 0.0f, 8.0f, "%.2f");
            ImGui::SliderFloat("Emission scale", &override.emissionScale, 0.0f, 8.0f, "%.2f");
            ImGui::SliderFloat("Env strength scale", &override.envScale, 0.0f, 4.0f, "%.2f");
            ImGui::SliderFloat("Metalness scale", &override.metallicScale, 0.0f, 8.0f, "%.2f");
            settingHint("Scales curated metalness, not an override. Above zero it tints Rf0 toward "
                        "albedo, which is what makes the specular factor chromatic - every stock "
                        "material here is dielectric.");
            ImGui::TreePop();
        }
    }
}

void Editor::graphicsCommitFooter() {
    auto &options = _engine._options.graphics;
    auto changed = _engine.stagedGraphicsChanges();

    ImGui::Separator();
    if (ImGui::Button("Save settings")) {
        // Applied and written, but not read back out of the live options: a
        // staged commit now lands at the rebuild point rather than inside this
        // call, so the live struct is still a frame behind when this line runs.
        // Saving it would write the values the window was opened with.
        //
        // What goes to the file is what Apply will install - the live options
        // for everything live, overlaid with the staged values for everything
        // that needs a rebuild - which is the same config either way once the
        // next frame starts.
        _engine.applyStagedGraphics();
        graphics::GraphicsOptions committed = options;
        graphics::copyGraphicsOptions(_engine.stagedGraphicsOptions(), committed,
                                      graphics::OptionApply::Reapply);
        _settingsSaveSucceeded = saveGraphicsOptions(committed, _settingsSaveStatus);
        if (_settingsSaveSucceeded) {
            _settingsSaveStatus = "Settings saved to reone.cfg.";
        }
    }
    settingHint("Writes the owned keys of reone.cfg, leaving every foreign line alone. Anything "
                "staged is applied too, so the file matches the frame that follows.");
    ImGui::SameLine();
    ImGui::BeginDisabled(changed.empty());
    if (ImGui::Button("Apply")) {
        _engine.applyStagedGraphics();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        _engine.revertStagedGraphics();
    }
    ImGui::EndDisabled();
    if (changed.empty()) {
        ImGui::TextDisabled("Nothing staged.");
    } else {
        // Named, because a staged control may be in any tab of this window -
        // the anti-aliasing slot is in Renderer, the resolution in Quality -
        // and the button that commits it is down here.
        std::string pending = "Pending:";
        for (const auto &name : changed) {
            pending += " " + name;
        }
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", pending.c_str());
    }
    if (!_settingsSaveStatus.empty()) {
        ImGui::TextColored(_settingsSaveSucceeded ? ImVec4(0.3f, 0.9f, 0.4f, 1.0f) : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "%s", _settingsSaveStatus.c_str());
    }
}

void Editor::renderModeCombo() {
    auto &staged = _engine.stagedGraphicsOptions();

    // Ordered as the enum is, so the index is the value.
    static const char *kModeNames[] = {"Retro", "PBR", "Path tracing"};
    // The staged value is what the control shows - the value Apply would
    // install, which is the only way a mode reaches the running frame.
    int modeIndex = static_cast<int>(staged.mode);
    if (ImGui::Combo("Render mode", &modeIndex, kModeNames, IM_ARRAYSIZE(kModeNames))) {
        // Through the same setter the console reaches, rather than deciding
        // anything here. It parses the written form, asks the registry whether
        // the change is live or needs a rebuild, and writes the struct that
        // answer names.
        //
        // This widget used to carry its own copy of that rule - retro and PBR
        // switch live, anything touching path tracing stages - and wrote the
        // live options itself when it judged a change free. It is not free:
        // retro allocates neither the tracing channel images nor the composite,
        // so a live switch into PBR shaded into images that were never
        // allocated and took the process down. Fixing the registry did nothing
        // for anyone using this window, because the copy here still had the old
        // answer. One implementation is the fix; the crash was the duplicate.
        _engine.setGraphicsOption(
            "mode", graphics::renderModeName(static_cast<graphics::RenderMode>(modeIndex)));
    }
    settingHint("Staged: a render mode decides what the pipeline allocates - whether the tracer "
                "exists, whether the tracing channels do, what format the scene output carries - "
                "and all of it is fixed when the pipeline is built. Apply is at the bottom of "
                "this window.\n\n"
                "Primary visibility is rasterized in every mode; this selects who shades it. "
                "The anti-aliasing slot does not follow the mode here - only --mode at startup "
                "defaults it - so set it yourself if you are comparing frames.");
}

void Editor::graphicsReapplySection() {
    auto &staged = _engine.stagedGraphicsOptions();

    ImGui::SeparatorText("Requires Apply");

    // Powers of two above 1024, which is exactly what the --shadowres exponent
    // can express. A free slider could stage a value the console could neither
    // report nor reproduce, and the two have to agree on what is set.
    static const char *kShadowResolutions[] = {"1024", "2048", "4096", "8192"};
    int shadowExponent = 0;
    while (shadowExponent < 3 && (1 << (10 + shadowExponent)) < staged.shadowResolution) {
        ++shadowExponent;
    }
    if (ImGui::Combo("Shadow resolution", &shadowExponent, kShadowResolutions,
                     IM_ARRAYSIZE(kShadowResolutions))) {
        staged.shadowResolution = 1 << (10 + shadowExponent);
    }
    // The point cubes are six faces each and there are many more of them, so
    // they carry their own resolution rather than following the cascades'.
    int pointExponent = 0;
    while (pointExponent < 3 && (1 << (10 + pointExponent)) < staged.pointShadowResolution) {
        ++pointExponent;
    }
    if (ImGui::Combo("Point shadow resolution", &pointExponent, kShadowResolutions,
                     IM_ARRAYSIZE(kShadowResolutions))) {
        staged.pointShadowResolution = 1 << (10 + pointExponent);
    }
    settingHint("These change what the pipeline allocates, so they are edited here and take effect "
                "on Apply, not as you drag.");

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
    std::string current = std::to_string(staged.width) + "x" + std::to_string(staged.height);
    if (ImGui::BeginCombo("Preset", current.c_str())) {
        for (const auto &res : kResolutions) {
            bool selected = res.width == staged.width && res.height == staged.height;
            if (ImGui::Selectable(res.name, selected)) {
                staged.width = res.width;
                staged.height = res.height;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::InputInt("Width", &staged.width);
    ImGui::InputInt("Height", &staged.height);
    staged.width = std::max(1, staged.width);
    staged.height = std::max(1, staged.height);
    ImGui::Checkbox("V-sync", &staged.vsync);
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

namespace {

/**
 * Rows from the renderer's admitted set rather than from the scene graph.
 *
 * What is in the graph and what reached the frame are different questions, and
 * for tracing work the second is the one worth asking: an object missing here
 * was culled or never registered, which the graph cannot tell you. The trade
 * is that the set is one frame old - see the note in Editor::drawObjects.
 */
class GpuSceneObjectSource : public IObjectSource {
public:
    GpuSceneObjectSource(Editor &editor, scene::SceneGraphs &graphs) :
        _editor(editor),
        _graphs(graphs) {
    }

    std::vector<ObjectRow> rows(const std::string &sceneName) override {
        _entries.clear();
        auto &graph = _graphs.get(sceneName);
        auto &scene = graph.gpuScene();
        _scene = &scene;
        _materials = &scene.traceMaterials();

        std::vector<ObjectRow> result;
        for (const auto &object : scene.objects()) {
            auto entry = makeObjectEntryView(graph, scene, object);
            ObjectRow row;
            row.id = entry.id;
            row.groupKey = entry.root;
            row.groupId = entry.root ? entry.root->id().index : 0;
            row.groupLabel = groupLabelFor(graph, entry);
            row.model = std::string(entry.modelName);
            row.name = std::string(entry.nodeName);
            row.kind = entry.kind;
            result.push_back(std::move(row));
            _entries.push_back(std::move(entry));
        }
        return result;
    }

    std::string summary(const std::string &sceneName) override {
        auto &scene = _graphs.get(sceneName).gpuScene();
        return "objects " + std::to_string(scene.counts().entries);
    }

    bool isEnabled(const ObjectRow &row) override {
        return _scene && _scene->isObjectEnabled(row.id);
    }

    void setEnabled(const ObjectRow &row, bool enabled) override {
        if (_scene) {
            _scene->setObjectEnabled(row.id, enabled);
        }
    }

    int extraColumnCount() const override { return 2; }

    void setupExtraColumns() override {
        ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthFixed, 158.0f);
        ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthFixed, 130.0f);
    }

    void drawExtraColumns(const ObjectRow &row) override {
        const ObjectEntryView *entry = find(row);
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(entry ? entry->material : "-");
        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(entry ? entry->classification.c_str() : "");
    }

    void drawGroupExtras(const std::vector<const ObjectRow *> &rows) override {
        // Instance counts belong on the group, not on its rows: grass is one
        // entry holding 1482 clusters, so a per-row column would read "1" and
        // hide the only number that matters for it.
        size_t particles = 0;
        size_t clusters = 0;
        for (const ObjectRow *row : rows) {
            if (const ObjectEntryView *entry = find(*row)) {
                particles += entry->particles;
                clusters += entry->clusters;
            }
        }
        if (clusters == 0 && particles == 0) {
            return;
        }
        ImGui::TableSetColumnIndex(3);
        ImGui::TextDisabled("%zu %s",
                            clusters != 0 ? clusters : particles,
                            clusters != 0 ? "clusters" : "particles");
    }

    void drawRowContextMenu(const ObjectRow &row) override {
        if (!_materials) {
            return;
        }
        // Right-click classifies: the mechanical per-level pass, written
        // straight to trace-classes.txt.
        if (!ImGui::BeginPopupContextItem("##classify")) {
            return;
        }
        auto curated = _materials->curatedFor(row.model, row.name);
        auto item = [this, &row, &curated](const char *label, scene::TraceClass klass) {
            if (ImGui::MenuItem(label, nullptr, curated.klass == klass)) {
                curated.klass = curated.klass == klass ? scene::TraceClass::Default : klass;
                _materials->setCurated(row.model, row.name, curated);
            }
        };
        ImGui::TextDisabled("Classify");
        ImGui::Separator();
        item("Prelit (fullbright, casts nothing)", scene::TraceClass::Prelit);
        item("Emissive (glows and casts)", scene::TraceClass::Emissive);
        item("None (strip selfIllum)", scene::TraceClass::None);
        ImGui::Separator();
        if (ImGui::MenuItem("Edit material...")) {
            _editor.openMaterialEditor(row.model, row.name, curated);
        }
        ImGui::EndPopup();
    }

    scene::TraceMaterialOverrides *materials() { return _materials; }

private:
    Editor &_editor;
    scene::SceneGraphs &_graphs;
    scene::GpuScene *_scene {nullptr};
    scene::TraceMaterialOverrides *_materials {nullptr};
    std::vector<ObjectEntryView> _entries;

    static std::string groupLabelFor(const scene::ISceneGraph &graph, const ObjectEntryView &entry) {
        if (entry.root) {
            std::string label(graph.nameText(entry.root->nameIds().model));
            return label.empty() ? "[unnamed model]" : label;
        }
        if (entry.kind == std::string_view("grass")) {
            return "[grass]";
        }
        if (entry.kind == std::string_view("particles")) {
            return "[emitters]";
        }
        return "[rootless]";
    }

    const ObjectEntryView *find(const ObjectRow &row) const {
        for (const auto &entry : _entries) {
            if (entry.id == row.id) {
                return &entry;
            }
        }
        return nullptr;
    }
};

} // namespace

void Editor::drawObjects() {
    dockNext();
    // Editor::update runs before SceneGraph::render resets and fills the
    // GpuScene, so the source reads the previous frame's complete snapshot.
    // Reading it during render would expose a partial frame.
    GpuSceneObjectSource source {*this, _engine._sceneModule->graphs()};
    _objectsPanel.draw(_engine._sceneModule->graphs(), source, _showObjects);
    if (auto *materials = source.materials()) {
        drawMaterialEditor(*materials);
    }
}

void Editor::drawMaterialEditor(scene::TraceMaterialOverrides &materials) {
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
        _materialEdit.klass = static_cast<scene::TraceClass>(klass);
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
        _materialEdit = scene::CuratedMaterial();
        changed = true;
    }
    if (changed) {
        materials.setCurated(_materialEditModel, _materialEditNode, _materialEdit);
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

    auto preview = pipeline->renderTargetPreview(selected->name, _rtMode, _rtScale);
    if (preview) {
        ImGui::Image(reinterpret_cast<ImTextureID>(preview),
                     ImVec2(kPreviewWidth, kPreviewHeight));
    }
    ImGui::End();
}

void Editor::applyPendingTransition() {
    if (!_pendingWarp.empty()) {
        auto target = std::move(_pendingWarp);
        _pendingWarp.clear();
        // The render-target viewer keeps a raw scene texture pointer. Loading
        // a module destroys that scene before its loading screen presents.
        if (_engine._game) {
            _engine._game->loadModule(target);
        }
        return;
    }
    if (!_pendingLoadGame.empty()) {
        auto target = std::move(_pendingLoadGame);
        _pendingLoadGame.clear();
        if (_engine._game) {
            // A slot is identified by a descriptor now, not a directory name -
            // see 8a27b7608's neighbour bb3b3493f. The panel still lists names
            // because that is what a person reads, so resolve through the
            // game's own enumeration rather than rebuilding the descriptor
            // here; that keeps one definition of what a slot is.
            for (const auto &save : _engine._game->savedGames()) {
                if (save.descriptor.directory.filename().string() == target) {
                    _engine._game->loadGame(save.descriptor);
                    break;
                }
            }
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
            // Flying the scene is how anything view-dependent gets checked -
            // shadow direction, reflections, the sky, the debug overlay - so
            // it belongs a click away rather than behind the console. WASD
            // moves, Q/Z rise and fall, Shift doubles the speed, the mouse
            // looks.
            auto &game = _engine._game;
            const bool freeCamera =
                game && game->cameraType() == game::CameraType::FirstPerson;
            if (ImGui::MenuItem("Free camera", "WASD/QZ", freeCamera, game != nullptr)) {
                if (!game->setFreeCameraEnabled(!freeCamera)) {
                    _freeCameraUnavailable = true;
                }
            }
            ImGui::Separator();
            ImGui::MenuItem("2DA", nullptr, &_showTwoDa);
            ImGui::MenuItem("Objects", nullptr, &_showObjects);
            ImGui::MenuItem("Render targets", nullptr, &_showRenderTargets);
            ImGui::MenuItem("Graphics settings", nullptr, &_showGraphicsSettings);
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
    if (_freeCameraUnavailable) {
        ImGui::OpenPopup("No free camera");
        _freeCameraUnavailable = false;
    }
    if (ImGui::BeginPopupModal("No free camera", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Load a module and be in game first - there is nothing to fly.");
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    dockSpace();

    if (_showRenderTargets) {
        renderTargets();
    } else {
    }

    if (_showObjects) {
        drawObjects();
    }

    if (_showWarp) {
        warp();
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
