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

#pragma once

#include <wx/wxprec.h>

#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include "reone/system/types.h"

namespace reone {

struct WindowID {
    static constexpr wxWindowID gameDir = wxID_HIGHEST + 1;
    static constexpr wxWindowID launch = wxID_HIGHEST + 2;
    static constexpr wxWindowID saveConfig = wxID_HIGHEST + 3;
};

class LauncherFrame : public wxFrame {
public:
    LauncherFrame();

private:
    struct Configuration {
        std::string gameDir;
        bool devMode {true};
        int width {1024};
        int height {768};
        int winscale {100};
        bool fullscreen {false};
        bool vsync {false};
        bool grass {true};
        /**
         * "retro", "pbr" or "path-tracing", matching the engine's --mode. The
         * engine also reads "raster" as retro, so a config written before this
         * became one option still launches.
         */
        std::string mode {"pbr"};
        int ptspp {8};
        bool ssao {true};
        bool ssr {true};
        /** "off", "fxaa", "fsr" or "dlssrr", matching the engine's --antialiasing. */
        std::string antialiasing {"fxaa"};
        /**
         * "dlaa", "quality", "balanced", "performance" or "ultraperformance".
         *
         * Under DLSS this is what sets the render resolution - the engine reads
         * a ratio out of it and ignores renderScale entirely - so a launcher
         * that offered only the FSR slider left DLSS users with no way to
         * render below native and a dial that did nothing.
         */
        std::string dlssMode {"dlaa"};
        /** Raster and trace resolution as a fraction of display when FSR runs. */
        float renderScale {1.0f};
        float sharpness {0.0f};
        int texQuality {0};
        int shadowres {1};
        int anisofilter {2};
        int drawdist {64};
        int musicvol {85};
        int voicevol {85};
        int soundvol {85};
        int movievol {85};
        int logsev {static_cast<int>(LogSeverity::Info)};
        int logch {static_cast<int>(LogChannel::Global)};
    } _config;

    wxTextCtrl *_textCtrlGameDir;
    wxCheckBox *_checkBoxDev;
    wxChoice *_choiceResolution;
    wxChoice *_choiceWinScale;
    wxChoice *_choiceRenderer;
    wxChoice *_choiceTextureQuality;
    wxChoice *_choiceShadowResolution;
    wxChoice *_choiceAnisoFilter;
    wxChoice *_choicePathTracingSamples;
    wxSlider *_sliderDrawDistance;
    wxSlider *_sliderRenderScale;
    wxCheckBox *_checkBoxFullscreen;
    wxCheckBox *_checkBoxVSync;
    wxCheckBox *_checkBoxGrass;
    wxCheckBox *_checkBoxSSAO;
    wxCheckBox *_checkBoxSSR;
    wxChoice *_choiceAntiAliasing;
    wxChoice *_choiceDlssMode;
    wxSlider *_sliderSharpness;
    wxSlider *_sliderVolumeMusic;
    wxSlider *_sliderVolumeVoice;
    wxSlider *_sliderVolumeSound;
    wxSlider *_sliderVolumeMovie;
    wxChoice *_choiceLogSeverity;
    wxCheckListBox *_checkListBoxLogChannels;

    void OnLaunch(wxCommandEvent &event);
    void OnSaveConfig(wxCommandEvent &event);
    void OnGameDirLeftDown(wxMouseEvent &event);

    /** Grey out the options unavailable to the chosen renderer. */
    void UpdateRendererDependentControls();

    void LoadConfiguration();
    void SaveConfiguration();
};

} // namespace reone
