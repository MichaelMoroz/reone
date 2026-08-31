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

#include "objectspanel.h"

#include "reone/scene/graphs.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>

namespace reone {

namespace {

struct Group {
    const void *key {nullptr};
    uint32_t id {0};
    std::string label;
    std::vector<const ObjectRow *> rows;
};

bool containsInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

void rightAligned(const std::string &text) {
    float width = ImGui::CalcTextSize(text.c_str()).x;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
    }
    ImGui::TextUnformatted(text.c_str());
}

} // namespace

void ObjectsPanel::draw(scene::SceneGraphs &graphs, IObjectSource &source, bool &open) {
    ImGui::SetNextWindowSize(ImVec2(700, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Objects", &open, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGui::End();
        return;
    }

    auto sceneNames = graphs.sceneNames();
    if (sceneNames.empty()) {
        ImGui::TextUnformatted("No scenes registered.");
        ImGui::End();
        return;
    }
    if (_scene.empty() || sceneNames.count(_scene) == 0) {
        // The world scene is what anyone opening this wants; the rest are GUI
        // sub-scenes that happen to sort first.
        auto main = sceneNames.find("main");
        _scene = main != sceneNames.end() ? *main : *sceneNames.begin();
    }
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::BeginCombo("##objects-scene", _scene.c_str())) {
        for (const auto &name : sceneNames) {
            if (ImGui::Selectable(name.c_str(), name == _scene)) {
                _scene = name;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("scene");

    std::string summary = source.summary(_scene);
    if (!summary.empty()) {
        ImGui::TextUnformatted(summary.c_str());
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##objects-filter", "Filter model or node", _filter, sizeof(_filter));
    ImGui::Spacing();

    auto rows = source.rows(_scene);
    std::string_view filter(_filter);

    // Grouped in encounter order, then sorted, so the tree is stable frame to
    // frame even though the source is free to report rows in any order.
    std::vector<Group> groups;
    for (const auto &row : rows) {
        if (!containsInsensitive(row.model, filter) && !containsInsensitive(row.name, filter)) {
            continue;
        }
        auto group = std::find_if(groups.begin(), groups.end(), [&row](const Group &candidate) {
            return candidate.key == row.groupKey && candidate.label == row.groupLabel;
        });
        if (group == groups.end()) {
            groups.push_back({row.groupKey, row.groupId, row.groupLabel, {}});
            group = std::prev(groups.end());
        }
        group->rows.push_back(&row);
    }
    std::sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
        return a.label < b.label;
    });
    // A resref names a model, not an instance, and an area places the same one
    // many times - danm14ab has eight c_khounda. The groups are already
    // distinct, being keyed separately, but on screen they were eight identical
    // rows. Disambiguate only where it is needed, so the common case stays
    // clean.
    for (size_t i = 0; i < groups.size();) {
        size_t j = i;
        while (j < groups.size() && groups[j].label == groups[i].label) {
            ++j;
        }
        if (j - i > 1) {
            for (size_t k = i; k < j; ++k) {
                groups[k].label += " #" + std::to_string(groups[k].id);
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
    const int extra = source.extraColumnCount();
    const int entriesColumn = 3 + extra;
    if (!ImGui::BeginTable("##objects-table", entriesColumn + 1, kTableFlags)) {
        ImGui::End();
        return;
    }
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 26.0f);
    ImGui::TableSetupColumn("Model / node", ImGuiTableColumnFlags_WidthFixed, 250.0f);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 64.0f);
    source.setupExtraColumns();
    ImGui::TableSetupColumn("Entries", ImGuiTableColumnFlags_WidthFixed, 68.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        auto &group = groups[groupIndex];
        ImGui::PushID(static_cast<int>(groupIndex));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        // The kill switch: unticking removes the object from the scene. The
        // group box drives all of its rows at once.
        bool groupEnabled = std::all_of(group.rows.begin(), group.rows.end(),
                                        [&source](const ObjectRow *row) {
                                            return source.isEnabled(*row);
                                        });
        if (ImGui::Checkbox("##group-on", &groupEnabled)) {
            for (const ObjectRow *row : group.rows) {
                source.setEnabled(*row, groupEnabled);
            }
        }
        ImGui::TableSetColumnIndex(1);
        // NoTreePushOnOpen because the indent a tree node pushes is window
        // state, not cell state: it stays applied while the rows underneath are
        // submitted and shifts every one of their cells, so the enable boxes
        // marched right along with the names. Only the name is nested here, and
        // it is nested by moving the cursor inside its own cell below.
        const bool openGroup = ImGui::TreeNodeEx(
            group.label.c_str(),
            ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen);
        source.drawGroupExtras(group.rows);
        ImGui::TableSetColumnIndex(entriesColumn);
        rightAligned(std::to_string(group.rows.size()));

        if (openGroup) {
            // Clipped: an area is tens of thousands of rows and only the open
            // group's visible slice is worth submitting.
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(group.rows.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const ObjectRow &row = *group.rows[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    bool enabled = source.isEnabled(row);
                    if (ImGui::Checkbox("##on", &enabled)) {
                        source.setEnabled(row, enabled);
                    }
                    ImGui::TableSetColumnIndex(1);
                    // Cell-local, so the nesting reads without disturbing the
                    // columns either side of it.
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetStyle().IndentSpacing);
                    const std::string &name = row.name;
                    if (name.empty()) {
                        ImGui::TextUnformatted("[unnamed]");
                    } else {
                        ImGui::TextUnformatted(name.c_str());
                    }
                    source.drawRowContextMenu(row);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(row.kind);
                    source.drawExtraColumns(row);
                    ImGui::PopID();
                }
            }
        }
        ImGui::PopID();
    }

    ImGui::EndTable();
    ImGui::End();
}

} // namespace reone
