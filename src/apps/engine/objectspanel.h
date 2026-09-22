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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reone {

namespace scene {
class SceneGraphs;
}

/**
 * One line in the Objects panel.
 *
 * Rows are grouped by @c groupKey rather than by label, because a resref names
 * a model and not an instance: an area places the same one many times, so two
 * groups sharing a label are still two groups.
 */
struct ObjectRow {
    /** Scene node index. Identity for enabling, and what disambiguates a name. */
    uint32_t id {UINT32_MAX};

    /** Opaque group identity. Rows sharing it belong under one tree node. */
    const void *groupKey {nullptr};
    /** Shown after the label when several groups share one. */
    uint32_t groupId {0};
    std::string groupLabel;

    /** Owning model, for a source that keys per-node data on it. */
    std::string model;
    std::string name;
    const char *kind {""};
};

/**
 * Where the Objects panel gets its rows, and what else it can say about them.
 *
 * The panel owns presentation only - the window, the scene selector, the
 * filter, grouping, and the enable column. What a row *is* differs by build:
 * walking the scene graph answers "what exists", while reading a renderer's
 * admitted set answers "what actually reached the frame". Both are useful and
 * neither is the panel's business.
 */
class IObjectSource {
public:
    virtual ~IObjectSource() = default;

    virtual std::vector<ObjectRow> rows(const std::string &sceneName) = 0;

    /** Shown beside the scene selector. Empty for none. */
    virtual std::string summary(const std::string &sceneName) { return {}; }

    virtual bool isEnabled(const ObjectRow &row) = 0;
    virtual void setEnabled(const ObjectRow &row, bool enabled) = 0;

    /** Columns between Kind and Entries. Must be consistent across a frame. */
    virtual int extraColumnCount() const { return 0; }
    /** Called inside BeginTable, in TableSetupColumn order. */
    virtual void setupExtraColumns() {}
    /** Fills exactly extraColumnCount() columns, from index 3. */
    virtual void drawExtraColumns(const ObjectRow &row) {}
    /** Same, for a group's own line; the rows behind it are passed in. */
    virtual void drawGroupExtras(const std::vector<const ObjectRow *> &rows) {}
    /** Submitted with the name cell as the popup's parent item. */
    virtual void drawRowContextMenu(const ObjectRow &row) {}
};

/**
 * The scene as a table: a tree of groups, each holding the rows a source
 * reported, with a checkbox that takes an object out of the scene.
 */
class ObjectsPanel {
public:
    /** @param open cleared when the window's close button is used. */
    void draw(scene::SceneGraphs &graphs, IObjectSource &source, bool &open);

private:
    std::string _scene;
    char _filter[128] {};
};

} // namespace reone
