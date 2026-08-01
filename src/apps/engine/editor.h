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
#include "reone/resource/id.h"
#include "reone/scene/gpuscene.h"

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
    std::string _objectsScene;
    char _objectsFilter[128] {};
    // Curated-material editor state: which key is open, live working copy.
    bool _showMaterialEditor {false};
    std::string _materialEditModel;
    std::string _materialEditNode;
    scene::CuratedMaterial _materialEdit;
    bool _objectsHideFullyCulled {false};

    void graphicsSettings();
    bool _showGraphicsSettings {false};
    float _pendingGrassDensity {-1.0f};

    void pathTracingSettings();
    bool _showPathTracing {false};
    int _pendingWidth {0};
    int _pendingHeight {0};
    int _pendingShadowResolution {0};
    bool _pendingVsync {true};
    /** Staged until Apply: the upscaler is built in the pipeline's init. */
    bool _pendingFsr {true};
    bool _pendingFsrInitialized {false};
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
