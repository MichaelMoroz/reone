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

#include "reone/graphics/texture.h"
#include "objectspanel.h"

#include "reone/resource/id.h"
#include "reone/scene/gpuscene.h"

#include <filesystem>

#include "imgui.h" // ImGuiID

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace reone {

namespace input {
struct Event;
}

class Engine;

// 2DA Table windows
class TableFilter {
public:
    void setupRowIndexFilter();
    void setupColumnFilters();

    bool nextRow();
    bool nextColumn(std::string_view column);

    void setNumColumns(size_t num) { _columnFilters.resize(num); }

private:
    std::string _rowIndexStr;
    std::optional<size_t> _rowIndex;
    std::vector<std::string> _columnFilters;

    size_t _nextRowIndex {0};
    size_t _nextColumnIndex {0};
};

class Editor {
public:
    Editor(Engine &engine, bool enabled) :
        _engine(engine),
        _enabled(enabled) {};
    bool handle(const input::Event &event);
    void update(float dt);
    // Module loads present loading-screen frames. Run them only after the
    // regular ImGui frame has been rendered and closed.
    void applyPendingTransition();

    bool isEnabled() const { return _enabled; }

    /**
     * Everything needed to name what is on screen, written to one folder.
     *
     * Exists because "that object over there" is not something a renderer can
     * be asked about, and guessing from a screenshot is how several changes
     * ended up aimed at the wrong geometry. A pixel gives a triangle id; the
     * dumped records turn that into a material and an object record; the object
     * list gives it a name.
     *
     * Public so the console can reach it - a button cannot be pressed by a
     * scripted run, and those are the runs that most need the evidence.
     *
     * Requested rather than taken: a button press and a console command both
     * land outside the render frame, where the renderer has nothing to read
     * back and refuses a flush. The engine performs it where its own screenshot
     * happens, with the finished frame still readable.
     */
    void requestSceneCapture() { _captureRequested = true; }
    void performPendingSceneCapture();

    /** Open the curated-material editor on one node. Called from the row menu. */
    void openMaterialEditor(std::string model, std::string node, scene::CuratedMaterial curated) {
        _showMaterialEditor = true;
        _materialEditModel = std::move(model);
        _materialEditNode = std::move(node);
        _materialEdit = std::move(curated);
    }

private:
    // Full-viewport dockspace, and the right-hand node new windows default into.
    void dockSpace();
    void dockNext();
    ImGuiID _rightDockId {0};
    bool _dockLayoutBuilt {false};

    // Render target viewer
    void renderTargets();
    bool _showRenderTargets {false};
    std::string _rtScene;
    std::string _rtTarget;
    int _rtMode {0};
    float _rtScale {1.0f};
    bool _rtAutoMode {true};

    // Objects viewer. It intentionally reads the last completed render
    // snapshot from update, before SceneGraph starts filling the next one.
    void drawObjects();
    void drawMaterialEditor(scene::TraceMaterialOverrides &materials);
    bool _showObjects {false};
    // Presentation lives in ObjectsPanel, which is shared verbatim with
    // upstream; this build only supplies the rows and its own columns.
    ObjectsPanel _objectsPanel;
    // Curated-material editor state: which key is open, live working copy.
    bool _showMaterialEditor {false};
    std::string _materialEditModel;
    std::string _materialEditNode;
    scene::CuratedMaterial _materialEdit;

    /**
     * One window for every graphics dial, traced or not.
     *
     * The split into a separate path-tracing panel stopped describing the
     * engine: the display transform is one common pass across the modes, and
     * the category overrides edit the shared material records the raster PBR
     * resolve reads too. What remains genuinely traced-only is a section, not
     * a window.
     */
    void graphicsSettings();
    bool _showGraphicsSettings {false};

    /**
     * One tab each, in the order a frame is usually worked on: what shades the
     * image, what it costs, the tracer, the dials only an artefact sends you
     * looking for, the diagnostic channels, and the shared material records.
     *
     * Their explanations are hover text rather than printed under each control
     * - see settingHint in the implementation.
     */
    void graphicsRendererTab();
    void graphicsQualityTab();
    void graphicsPathTracingTab();
    void graphicsAdvancedTab();
    void graphicsDebugViewSection();
    /** Opens the folder and writes everything that is not a debug channel. */
    bool beginSceneCapture();
    bool _captureRequested {false};
    /** Channel the next finished frame carries, or -1 when idle. */
    int _captureStep {-1};
    int _captureRestoreView {0};
    std::filesystem::path _captureDir;
    std::string _lastCapturePath;
    void graphicsMaterialsTab();

    /**
     * Save, Apply and Revert, pinned below the scrolling tabs.
     *
     * A staged control can be in any tab, so the row that commits them cannot
     * be part of the flow that scrolls one of them out of sight.
     */
    void graphicsCommitFooter();

    /**
     * Every control whose option changes what the pipeline allocates edits
     * Engine::stagedGraphicsOptions() rather than the live struct, and the one
     * Apply button in the footer copies the staged reapply fields across and
     * schedules the rebuild. The staging buffer lives on the Engine, not here,
     * because the console drives the same one - the two must not be separate
     * mechanisms.
     */
    void graphicsReapplySection();
    /** The one render-mode control; live or staged depending on the value. */
    void renderModeCombo();
    std::string _settingsSaveStatus;
    bool _settingsSaveSucceeded {false};

    void frameTimes();
    bool _showFrameTimes {false};

    void warp();
    bool _showWarp {false};
    char _warpFilter[64] {};
    std::string _pendingWarp;

    struct WarpTarget {
        std::string module;
        std::string title;
        std::string entryArea;
        std::string tag;
        std::string startMovie;
        size_t areaCount {0};
    };
    std::vector<WarpTarget> _warpTargets;
    bool _warpTargetsScanned {false};
    std::string _selectedWarp;
    void scanWarpTargets();

    struct SaveEntry {
        std::string directory;
        std::string name;
        std::string area;
        std::string module;
        uint32_t timePlayed {0};
    };
    std::vector<SaveEntry> _saves;
    bool _savesScanned {false};
    std::string _pendingLoadGame;
    void scanSaves();

    // 2DA List window
    void twoDa();
    bool _showTwoDa {false};

    struct TwoDaTableContext {
        bool show {false};
        TableFilter filter;
    };

    void twoDaRes(const resource::ResourceId &res, TwoDaTableContext &context);
    std::map<resource::ResourceId, TwoDaTableContext> _twoDaContext;

    // DearImGui Demo showcasing different widgets.
    void imGuiDemo();
    bool _showImGuiDemo {false};

private:
    Engine &_engine;
    bool _enabled {false};
};

} // namespace reone
