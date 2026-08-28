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

#include "frame.h"

#include "reone/graphics/types.h"

#include <algorithm>
#include <cmath>

using namespace boost::program_options;

using namespace reone::graphics;

namespace reone {

static const char kIconName[] = "reone";
static const char kConfigFilename[] = "reone.cfg";

LauncherFrame::LauncherFrame() :
    wxFrame(nullptr, wxID_ANY, "reone", wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE & ~(wxRESIZE_BORDER | wxMAXIMIZE_BOX | wxMINIMIZE_BOX)) {

#ifdef _WIN32
    SetIcon(wxIcon(kIconName));
#endif

    LoadConfiguration();

    // Setup controls

    auto labelGameDir = new wxStaticText(this, wxID_ANY, "Game Directory", wxDefaultPosition, wxDefaultSize);

    _textCtrlGameDir = new wxTextCtrl(this, WindowID::gameDir, _config.gameDir, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
    _textCtrlGameDir->Bind(wxEVT_LEFT_DOWN, &LauncherFrame::OnGameDirLeftDown, this, WindowID::gameDir);

    auto gameSizer = new wxBoxSizer(wxVERTICAL);
    gameSizer->Add(labelGameDir, wxSizerFlags(0).Expand());
    gameSizer->Add(_textCtrlGameDir, wxSizerFlags(0).Expand());

    _checkBoxDev = new wxCheckBox(this, wxID_ANY, "Developer Mode", wxDefaultPosition, wxDefaultSize);
    _checkBoxDev->SetValue(_config.devMode);

    // END Setup controls

    // Graphics

    // Render Resolution

    wxArrayString resChoices;
    resChoices.Add("800x600");
    resChoices.Add("1024x768");
    resChoices.Add("1280x720");
    resChoices.Add("1280x1024");
    resChoices.Add("1366x768");
    resChoices.Add("1600x900");
    resChoices.Add("1600x1200");
    resChoices.Add("1920x1080");
    resChoices.Add("2560x1440");
    resChoices.Add("3840x2160");

    int displayW, displayH;
    wxDisplaySize(&displayW, &displayH);
    std::string displayRes = std::to_string(displayW) + "x" + std::to_string(displayH);
    if (resChoices.Index(displayRes) == wxNOT_FOUND) {
        resChoices.Add(displayRes);
    }

    std::string configResolution(str(boost::format("%dx%d") % _config.width % _config.height));
    int resSelection = resChoices.Index(configResolution);
    if (resSelection == wxNOT_FOUND) {
        resChoices.Add(configResolution);
        resSelection = resChoices.GetCount() - 1;
    }

    auto labelResolution = new wxStaticText(this, wxID_ANY, "Render Resolution", wxDefaultPosition, wxDefaultSize);

    _choiceResolution = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, resChoices);
    _choiceResolution->SetSelection(resSelection);

    auto resSizer = new wxBoxSizer(wxVERTICAL);
    resSizer->Add(labelResolution, wxSizerFlags(0).Expand());
    resSizer->Add(_choiceResolution, wxSizerFlags(0).Expand());

    // END Render Resolution

    // Window Scale

    auto winScalesLabel = new wxStaticText(this, wxID_ANY, "Window Scale", wxDefaultPosition, wxDefaultSize);

    wxArrayString winScales;
    winScales.Add("100%");
    winScales.Add("125%");
    winScales.Add("150%");
    winScales.Add("175%");
    winScales.Add("200%");
    // The engine takes any percent from 1 to 400, so one set outside this
    // ladder is legal; offering it keeps this from silently snapping back to
    // 100% and writing that over the user's choice, the way the resolution list
    // below already avoids.
    auto winScaleText = str(boost::format("%d%%") % _config.winscale);
    if (winScales.Index(winScaleText) == wxNOT_FOUND) {
        winScales.Add(winScaleText);
    }
    const int winScaleSel = winScales.Index(winScaleText);
    _choiceWinScale = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, winScales);
    _choiceWinScale->SetSelection(winScaleSel);

    auto winScaleSizer = new wxBoxSizer(wxVERTICAL);
    winScaleSizer->Add(winScalesLabel, wxSizerFlags(0).Expand());
    winScaleSizer->Add(_choiceWinScale, wxSizerFlags(0).Expand());

    // END Window Scale

    // Renderer

    auto labelRenderer = new wxStaticText(this, wxID_ANY, "Renderer", wxDefaultPosition, wxDefaultSize);

    wxArrayString rendererChoices;
    rendererChoices.Add("Retro");
    rendererChoices.Add("PBR");
    rendererChoices.Add("Path tracing");

    _choiceRenderer = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, rendererChoices);
    // One three-way option, in the same order the engine's enum declares it.
    // "raster" is the old spelling of retro and still arrives from an existing
    // config; the engine reads it the same way.
    _choiceRenderer->SetSelection(_config.mode == "path-tracing" ? 2
                                  : _config.mode == "pbr"       ? 1
                                                                : 0);
    _choiceRenderer->Bind(wxEVT_COMMAND_CHOICE_SELECTED, [this](const wxCommandEvent &evt) {
        UpdateRendererDependentControls();
    });

    auto rendererSizer = new wxBoxSizer(wxVERTICAL);
    rendererSizer->Add(labelRenderer, wxSizerFlags(0).Expand());
    rendererSizer->Add(_choiceRenderer, wxSizerFlags(0).Expand());

    // END Renderer

    // Path tracing samples

    auto labelPathTracingSamples =
        new wxStaticText(this, wxID_ANY, "Samples Per Pixel", wxDefaultPosition, wxDefaultSize);

    // Powers of two rather than a free integer: cost is near linear in this
    // count and noise falls as its square root, so the useful settings are
    // spread across a doubling scale rather than adjacent values.
    wxArrayString pathTracingSampleChoices;
    for (const auto *count : {"1", "2", "4", "8", "16", "32", "64"}) {
        pathTracingSampleChoices.Add(count);
    }

    // The engine takes any count from 1 to 64, so one set in game need not be a
    // power of two. Offering it alongside the ladder keeps this from snapping to
    // 8 and writing that back over the user's choice.
    if (pathTracingSampleChoices.Index(std::to_string(_config.ptspp)) == wxNOT_FOUND) {
        pathTracingSampleChoices.Add(std::to_string(_config.ptspp));
    }

    _choicePathTracingSamples =
        new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, pathTracingSampleChoices);
    _choicePathTracingSamples->SetStringSelection(std::to_string(_config.ptspp));

    auto pathTracingSizer = new wxBoxSizer(wxVERTICAL);
    pathTracingSizer->Add(labelPathTracingSamples, wxSizerFlags(0).Expand());
    pathTracingSizer->Add(_choicePathTracingSamples, wxSizerFlags(0).Expand());

    // END Path tracing samples

    // Texture Quality

    auto labelTextureQuality = new wxStaticText(this, wxID_ANY, "Texture Quality", wxDefaultPosition, wxDefaultSize);

    wxArrayString textureQualityChoices;
    textureQualityChoices.Add("High");
    textureQualityChoices.Add("Medium");
    textureQualityChoices.Add("Low");

    _choiceTextureQuality = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, textureQualityChoices);
    _choiceTextureQuality->SetSelection(_config.texQuality);

    auto textureQualitySizer = new wxBoxSizer(wxVERTICAL);
    textureQualitySizer->Add(labelTextureQuality, wxSizerFlags(0).Expand());
    textureQualitySizer->Add(_choiceTextureQuality, wxSizerFlags(0).Expand());

    // END Texture Quality

    // Shadow Map Resolution

    // One entry per step the engine accepts - it reads this as an exponent and
    // clamps to 0..3. A config at 3 used to select nothing in a three-item
    // list, and wxChoice's "nothing" is -1, which went back out as
    // shadowres=-1 and came back as 1024.
    wxArrayString shadowResChoices;
    shadowResChoices.Add("1024");
    shadowResChoices.Add("2048");
    shadowResChoices.Add("4096");
    shadowResChoices.Add("8192");

    auto labelShadowResolution = new wxStaticText(this, wxID_ANY, "Shadow Map Resolution", wxDefaultPosition, wxDefaultSize);

    _choiceShadowResolution = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, shadowResChoices);
    _choiceShadowResolution->SetSelection(
        std::clamp(_config.shadowres, 0, static_cast<int>(shadowResChoices.GetCount()) - 1));

    auto shadowResSizer = new wxBoxSizer(wxVERTICAL);
    shadowResSizer->Add(labelShadowResolution, wxSizerFlags(0).Expand());
    shadowResSizer->Add(_choiceShadowResolution, wxSizerFlags(0).Expand());

    // END Shadow Map Resolution

    // Anisotropic Filtering

    wxArrayString anisoFilterChoices;
    anisoFilterChoices.Add("Off");
    anisoFilterChoices.Add("2x");
    anisoFilterChoices.Add("4x");
    anisoFilterChoices.Add("8x");
    anisoFilterChoices.Add("16x");

    auto labelAnisoFilter = new wxStaticText(this, wxID_ANY, "Anisotropic Filtering", wxDefaultPosition, wxDefaultSize);

    _choiceAnisoFilter = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, anisoFilterChoices);
    _choiceAnisoFilter->SetSelection(
        std::clamp(_config.anisofilter, 0, static_cast<int>(anisoFilterChoices.GetCount()) - 1));

    auto anisoFilterSizer = new wxBoxSizer(wxVERTICAL);
    anisoFilterSizer->Add(labelAnisoFilter, wxSizerFlags(0).Expand());
    anisoFilterSizer->Add(_choiceAnisoFilter, wxSizerFlags(0).Expand());

    // END Anisotropic Filtering

    // Object Draw Distance

    auto labelDrawDistance = new wxStaticText(this, wxID_ANY, "Object Draw Distance", wxDefaultPosition, wxDefaultSize);
    _sliderDrawDistance = new wxSlider(this, wxID_ANY, _config.drawdist, 32, 128, wxDefaultPosition, wxDefaultSize);

    auto drawDistanceSizer = new wxBoxSizer(wxVERTICAL);
    drawDistanceSizer->Add(labelDrawDistance, wxSizerFlags(0).Expand());
    drawDistanceSizer->Add(_sliderDrawDistance, wxSizerFlags(0).Expand());

    // END Object Draw Distance

    _checkBoxFullscreen = new wxCheckBox(this, wxID_ANY, "Enable Fullscreen", wxDefaultPosition, wxDefaultSize);
    _checkBoxFullscreen->SetValue(_config.fullscreen);

    _checkBoxVSync = new wxCheckBox(this, wxID_ANY, "Enable V-Sync", wxDefaultPosition, wxDefaultSize);
    _checkBoxVSync->SetValue(_config.vsync);

    _checkBoxGrass = new wxCheckBox(this, wxID_ANY, "Enable Grass", wxDefaultPosition, wxDefaultSize);
    _checkBoxGrass->SetValue(_config.grass);

    _checkBoxSSAO = new wxCheckBox(this, wxID_ANY, "Enable SSAO", wxDefaultPosition, wxDefaultSize);
    _checkBoxSSAO->SetValue(_config.ssao);

    _checkBoxSSR = new wxCheckBox(this, wxID_ANY, "Enable SSR", wxDefaultPosition, wxDefaultSize);
    _checkBoxSSR->SetValue(_config.ssr);


    // Anti-aliasing

    auto labelAntiAliasing = new wxStaticText(this, wxID_ANY, "Anti-aliasing",
                                              wxDefaultPosition, wxDefaultSize);

    wxArrayString antiAliasingChoices;
    antiAliasingChoices.Add("Off");
    antiAliasingChoices.Add("FXAA");
    antiAliasingChoices.Add("FSR 2 (NativeAA)");
    antiAliasingChoices.Add("DLSS Ray Reconstruction");

    _choiceAntiAliasing = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                       antiAliasingChoices);
    // Every value the engine accepts needs an entry here. A value this list
    // cannot represent collapses to index 0 and is written back as "off" on
    // save, silently discarding a setting made in game - which is exactly what
    // happened to dlssrr while this was a three-item list.
    _choiceAntiAliasing->SetSelection(_config.antialiasing == "dlssrr" ? 3
                                      : _config.antialiasing == "fsr"  ? 2
                                      : _config.antialiasing == "fxaa" ? 1
                                                                       : 0);
    _choiceAntiAliasing->Bind(wxEVT_COMMAND_CHOICE_SELECTED, [this](const wxCommandEvent &evt) {
        UpdateRendererDependentControls();
    });

    auto antiAliasingSizer = new wxBoxSizer(wxVERTICAL);
    antiAliasingSizer->Add(labelAntiAliasing, wxSizerFlags(0).Expand());
    antiAliasingSizer->Add(_choiceAntiAliasing, wxSizerFlags(0).Expand());

    // END Anti-aliasing

    // DLSS quality mode

    auto labelDlssMode = new wxStaticText(this, wxID_ANY, "DLSS Quality Mode",
                                          wxDefaultPosition, wxDefaultSize);

    // Named modes rather than a scale slider, because that is the only way DLSS
    // expresses a render resolution: the engine turns the mode into a ratio
    // (DLAA 1.00, Quality 0.67, Balanced 0.58, Performance 0.50, Ultra
    // Performance 0.33) and does not read renderScale at all while DLSS holds
    // the slot.
    wxArrayString dlssModeChoices;
    dlssModeChoices.Add("DLAA (native)");
    dlssModeChoices.Add("Quality");
    dlssModeChoices.Add("Balanced");
    dlssModeChoices.Add("Performance");
    dlssModeChoices.Add("Ultra Performance");

    _choiceDlssMode = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   dlssModeChoices);
    _choiceDlssMode->SetSelection(_config.dlssMode == "ultraperformance" ? 4
                                  : _config.dlssMode == "performance"   ? 3
                                  : _config.dlssMode == "balanced"      ? 2
                                  : _config.dlssMode == "quality"       ? 1
                                                                        : 0);

    auto dlssModeSizer = new wxBoxSizer(wxVERTICAL);
    dlssModeSizer->Add(labelDlssMode, wxSizerFlags(0).Expand());
    dlssModeSizer->Add(_choiceDlssMode, wxSizerFlags(0).Expand());

    // END DLSS quality mode

    // FSR render scale

    auto labelRenderScale = new wxStaticText(this, wxID_ANY, "FSR Render Scale",
                                             wxDefaultPosition, wxDefaultSize);
    _sliderRenderScale = new wxSlider(
        this, wxID_ANY, static_cast<int>(std::round(_config.renderScale * 100.0f)), 25, 100,
        wxDefaultPosition, wxDefaultSize);

    auto renderScaleSizer = new wxBoxSizer(wxVERTICAL);
    renderScaleSizer->Add(labelRenderScale, wxSizerFlags(0).Expand());
    renderScaleSizer->Add(_sliderRenderScale, wxSizerFlags(0).Expand());

    // END FSR render scale

    // One dial, as the engine has: RCAS under FSR, an unsharp mask otherwise.
    // The old checkbox wrote a `sharpen` key the engine stopped parsing.
    _sliderSharpness = new wxSlider(this, wxID_ANY,
                                    static_cast<int>(_config.sharpness * 100.0f), 0, 100,
                                    wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL | wxSL_LABELS);

    UpdateRendererDependentControls();

    auto graphicsSizer = new wxStaticBoxSizer(wxVERTICAL, this, "Graphics");
    graphicsSizer->Add(resSizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(winScaleSizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(rendererSizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(pathTracingSizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(textureQualitySizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(shadowResSizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(anisoFilterSizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(drawDistanceSizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(_checkBoxFullscreen, wxSizerFlags(0).Expand());
    graphicsSizer->Add(_checkBoxVSync, wxSizerFlags(0).Expand());
    graphicsSizer->Add(_checkBoxGrass, wxSizerFlags(0).Expand());
    graphicsSizer->Add(_checkBoxSSAO, wxSizerFlags(0).Expand());
    graphicsSizer->Add(_checkBoxSSR, wxSizerFlags(0).Expand());
    graphicsSizer->Add(antiAliasingSizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(dlssModeSizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(renderScaleSizer, wxSizerFlags(0).Expand());
    graphicsSizer->Add(new wxStaticText(this, wxID_ANY, "Sharpness"), wxSizerFlags(0).Expand());
    graphicsSizer->Add(_sliderSharpness, wxSizerFlags(0).Expand());

    // END Graphics

    // Audio

    auto labelVolumeMusic = new wxStaticText(this, wxID_ANY, "Music Volume", wxDefaultPosition, wxDefaultSize);
    _sliderVolumeMusic = new wxSlider(this, wxID_ANY, _config.musicvol, 0, 100, wxDefaultPosition, wxDefaultSize);

    auto labelVolumeVoice = new wxStaticText(this, wxID_ANY, "Voice Volume", wxDefaultPosition, wxDefaultSize);
    _sliderVolumeVoice = new wxSlider(this, wxID_ANY, _config.voicevol, 0, 100, wxDefaultPosition, wxDefaultSize);

    auto labelVolumeSound = new wxStaticText(this, wxID_ANY, "Sound Volume", wxDefaultPosition, wxDefaultSize);
    _sliderVolumeSound = new wxSlider(this, wxID_ANY, _config.soundvol, 0, 100, wxDefaultPosition, wxDefaultSize);

    auto labelVolumeMovie = new wxStaticText(this, wxID_ANY, "Movie Volume", wxDefaultPosition, wxDefaultSize);
    _sliderVolumeMovie = new wxSlider(this, wxID_ANY, _config.movievol, 0, 100, wxDefaultPosition, wxDefaultSize);

    auto audioSizer = new wxStaticBoxSizer(wxVERTICAL, this, "Audio");
    audioSizer->Add(labelVolumeMusic, wxSizerFlags(0).Expand());
    audioSizer->Add(_sliderVolumeMusic, wxSizerFlags(0).Expand());
    audioSizer->Add(labelVolumeVoice, wxSizerFlags(0).Expand());
    audioSizer->Add(_sliderVolumeVoice, wxSizerFlags(0).Expand());
    audioSizer->Add(labelVolumeSound, wxSizerFlags(0).Expand());
    audioSizer->Add(_sliderVolumeSound, wxSizerFlags(0).Expand());
    audioSizer->Add(labelVolumeMovie, wxSizerFlags(0).Expand());
    audioSizer->Add(_sliderVolumeMovie, wxSizerFlags(0).Expand());

    // END Audio

    // Logging

    wxArrayString logSeverityChoices;
    logSeverityChoices.Add("Debug");
    logSeverityChoices.Add("Info");
    logSeverityChoices.Add("Warning");
    logSeverityChoices.Add("Error");
    logSeverityChoices.Add("None");

    auto labelLogSeverity = new wxStaticText(this, wxID_ANY, "Minimum Severity", wxDefaultPosition, wxDefaultSize);
    _choiceLogSeverity = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, logSeverityChoices);
    _choiceLogSeverity->SetSelection(_config.logsev);

    wxArrayString logChannelChoices;
    logChannelChoices.Add("Resources");
    logChannelChoices.Add("Resources (verbose)");
    logChannelChoices.Add("Graphics");
    logChannelChoices.Add("Audio");
    logChannelChoices.Add("GUI");
    logChannelChoices.Add("Perception");
    logChannelChoices.Add("Conversation");
    logChannelChoices.Add("Combat");
    logChannelChoices.Add("Script");
    logChannelChoices.Add("Script (verbose)");
    logChannelChoices.Add("Script (very verbose)");

    auto labelLogChannels = new wxStaticText(this, wxID_ANY, "Channels", wxDefaultPosition, wxDefaultSize);

    _checkListBoxLogChannels = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, logChannelChoices);
    _checkListBoxLogChannels->Check(0, _config.logch & static_cast<int>(LogChannel::Resources));
    _checkListBoxLogChannels->Check(1, _config.logch & static_cast<int>(LogChannel::Resources2));
    _checkListBoxLogChannels->Check(2, _config.logch & static_cast<int>(LogChannel::Graphics));
    _checkListBoxLogChannels->Check(3, _config.logch & static_cast<int>(LogChannel::Audio));
    _checkListBoxLogChannels->Check(4, _config.logch & static_cast<int>(LogChannel::GUI));
    _checkListBoxLogChannels->Check(5, _config.logch & static_cast<int>(LogChannel::Perception));
    _checkListBoxLogChannels->Check(6, _config.logch & static_cast<int>(LogChannel::Conversation));
    _checkListBoxLogChannels->Check(7, _config.logch & static_cast<int>(LogChannel::Combat));
    _checkListBoxLogChannels->Check(8, _config.logch & static_cast<int>(LogChannel::Script));
    _checkListBoxLogChannels->Check(9, _config.logch & static_cast<int>(LogChannel::Script2));
    _checkListBoxLogChannels->Check(10, _config.logch & static_cast<int>(LogChannel::Script3));

    auto loggingSizer = new wxStaticBoxSizer(wxVERTICAL, this, "Logging");
    loggingSizer->Add(labelLogSeverity, wxSizerFlags(0).Expand());
    loggingSizer->Add(_choiceLogSeverity, wxSizerFlags(0).Expand());
    loggingSizer->Add(labelLogChannels, wxSizerFlags(0).Expand());
    loggingSizer->Add(_checkListBoxLogChannels, wxSizerFlags(0).Expand());

    // END Logging

    auto topSizer = new wxBoxSizer(wxHORIZONTAL);
    topSizer->Add(graphicsSizer, wxSizerFlags(1).Expand());
    topSizer->Add(audioSizer, wxSizerFlags(1).Expand());
    topSizer->Add(loggingSizer, wxSizerFlags(1).Expand());

    auto topSizer2 = new wxBoxSizer(wxVERTICAL);
    topSizer2->SetMinSize(640, 100);
    topSizer2->Add(gameSizer, wxSizerFlags(0).Expand().Border(wxALL, 3));
    topSizer2->Add(_checkBoxDev, wxSizerFlags(0).Expand().Border(wxALL, 3));
    topSizer2->Add(topSizer, wxSizerFlags(0).Expand().Border(wxALL, 3));
    topSizer2->Add(new wxButton(this, WindowID::launch, "Launch"), wxSizerFlags(0).Expand().Border(wxALL, 3));
    topSizer2->Add(new wxButton(this, WindowID::saveConfig, "Save Configuration"), wxSizerFlags(0).Expand().Border(wxALL, 3));

    SetSizerAndFit(topSizer2);

    Bind(wxEVT_BUTTON, &LauncherFrame::OnLaunch, this, WindowID::launch);
    Bind(wxEVT_BUTTON, &LauncherFrame::OnSaveConfig, this, WindowID::saveConfig);
}

void LauncherFrame::UpdateRendererDependentControls() {
    // Both backends now carry the whole chain - filters, retro forward shading,
    // and the screen-space effects - so nothing here depends on the backend any
    // more. Only SSAO and SSR remain conditional, and on the renderer rather
    // than the backend: they read the G-buffer, which the retro path does not
    // produce. Greyed out instead of hidden, so their saved values are still
    // visible and still written back.
    auto renderer = _choiceRenderer->GetStringSelection();
    bool pbr = renderer == "PBR";
    bool pathTracing = renderer == "Path tracing";
    _checkBoxSSAO->Enable(pbr);
    _checkBoxSSR->Enable(pbr);
    // Screen-space effects read a G-buffer the traced path never produces, and
    // the sample count means nothing to the two raster renderers.
    _choicePathTracingSamples->Enable(pathTracing);
    // Rendering below display resolution is meaningful only when an upscaler
    // owns the anti-aliasing slot, and each upscaler has its own dial for it:
    // FSR reads renderScale, DLSS reads dlssmode and ignores renderScale
    // entirely. So exactly one of the two is live, never both - the slider was
    // enabled under DLSS, where moving it did nothing. Greyed rather than
    // hidden, so the inactive one still shows what it will do if selected.
    const int aaSel = _choiceAntiAliasing->GetSelection();
    _sliderRenderScale->Enable(aaSel == 2);
    _choiceDlssMode->Enable(aaSel == 3);
}

void LauncherFrame::LoadConfiguration() {
    options_description options;
    options.add_options()                                                 //
        ("game", value<std::string>()->default_value(_config.gameDir))    //
        ("dev", value<bool>()->default_value(_config.devMode))            //
        ("width", value<int>()->default_value(_config.width))             //
        ("height", value<int>()->default_value(_config.height))           //
        ("winscale", value<int>()->default_value(_config.winscale))       //
        ("fullscreen", value<bool>()->default_value(_config.fullscreen))  //
        ("vsync", value<bool>()->default_value(_config.vsync))            //
        ("grass", value<bool>()->default_value(_config.grass))            //
        ("mode", value<std::string>()->default_value(_config.mode))        //
        ("ptspp", value<int>()->default_value(_config.ptspp))              //
        ("ssao", value<bool>()->default_value(_config.ssao))              //
        ("ssr", value<bool>()->default_value(_config.ssr))                //
        ("antialiasing", value<std::string>()->default_value(_config.antialiasing)) //
        ("dlssmode", value<std::string>()->default_value(_config.dlssMode))  //
        ("renderscale", value<float>()->default_value(_config.renderScale)) //
        ("sharpness", value<float>()->default_value(_config.sharpness))   //
        ("texquality", value<int>()->default_value(_config.texQuality))   //
        ("anisofilter", value<int>()->default_value(_config.anisofilter)) //
        ("shadowres", value<int>()->default_value(_config.shadowres))     //
        ("drawdist", value<float>()->default_value(_config.drawdist))     //
        ("musicvol", value<int>()->default_value(_config.musicvol))       //
        ("voicevol", value<int>()->default_value(_config.voicevol))       //
        ("soundvol", value<int>()->default_value(_config.soundvol))       //
        ("movievol", value<int>()->default_value(_config.movievol))       //
        ("logsev", value<int>()->default_value(_config.logsev))           //
        ("logch", value<int>()->default_value(_config.logch));

    variables_map vars;
    if (!std::filesystem::exists(kConfigFilename)) {
        return;
    }

    store(parse_config_file<char>(kConfigFilename, options, true), vars);
    notify(vars);

    _config.gameDir = vars["game"].as<std::string>();
    _config.devMode = vars["dev"].as<bool>();
    _config.width = vars["width"].as<int>();
    _config.height = vars["height"].as<int>();
    _config.winscale = vars["winscale"].as<int>();
    _config.fullscreen = vars["fullscreen"].as<bool>();
    _config.vsync = vars["vsync"].as<bool>();
    _config.grass = vars["grass"].as<bool>();
    _config.mode = vars["mode"].as<std::string>();
    _config.ptspp = std::max(1, vars["ptspp"].as<int>());
    _config.ssao = vars["ssao"].as<bool>();
    _config.ssr = vars["ssr"].as<bool>();
    _config.antialiasing = vars["antialiasing"].as<std::string>();
    _config.dlssMode = vars["dlssmode"].as<std::string>();
    _config.renderScale = std::clamp(vars["renderscale"].as<float>(), 0.25f, 1.0f);
    _config.sharpness = vars["sharpness"].as<float>();
    _config.texQuality = vars["texquality"].as<int>();
    _config.shadowres = vars["shadowres"].as<int>();
    _config.anisofilter = vars["anisofilter"].as<int>();
    // Float on the engine's side. The slider is whole numbers, so a fractional
    // value set from the console rounds here rather than refusing to parse -
    // which is what value<int>() did, throwing before the window ever opened.
    _config.drawdist = static_cast<int>(std::lround(vars["drawdist"].as<float>()));
    _config.musicvol = vars["musicvol"].as<int>();
    _config.voicevol = vars["voicevol"].as<int>();
    _config.soundvol = vars["soundvol"].as<int>();
    _config.movievol = vars["movievol"].as<int>();
    _config.logsev = vars["logsev"].as<int>();
    _config.logch = vars["logch"].as<int>();
}

void LauncherFrame::OnLaunch(wxCommandEvent &event) {
    SaveConfiguration();

    std::string exe("engine");
#ifndef _WIN32
    exe.insert(0, "./");
#endif

    system(exe.c_str());

    Close(true);
}

void LauncherFrame::SaveConfiguration() {
    static std::set<std::string> recognized {
        "game=",
        "dev=",
        "width=",
        "height=",
        "winscale=",
        "fullscreen=",
        "vsync=",
        "grass=",
        "mode=",
        "ptspp=",
        "ssao=",
        "ssr=",
        "antialiasing=",
        "dlssmode=",
        "renderscale=",
        "sharpness=",
        "texquality=",
        "anisofilter=",
        "shadowres=",
        "drawdist=",
        "musicvol=",
        "voicevol=",
        "soundvol=",
        "movievol=",
        "logsev=",
        "logch="};

    std::string resolution(_choiceResolution->GetStringSelection());
    std::vector<std::string> tokens;
    boost::split(tokens, resolution, boost::is_any_of("x"), boost::token_compress_on);

    int logch = static_cast<int>(LogChannel::Global);
    if (_checkListBoxLogChannels->IsChecked(0)) {
        logch |= static_cast<int>(LogChannel::Resources);
    }
    if (_checkListBoxLogChannels->IsChecked(1)) {
        logch |= static_cast<int>(LogChannel::Resources2);
    }
    if (_checkListBoxLogChannels->IsChecked(2)) {
        logch |= static_cast<int>(LogChannel::Graphics);
    }
    if (_checkListBoxLogChannels->IsChecked(3)) {
        logch |= static_cast<int>(LogChannel::Audio);
    }
    if (_checkListBoxLogChannels->IsChecked(4)) {
        logch |= static_cast<int>(LogChannel::GUI);
    }
    if (_checkListBoxLogChannels->IsChecked(5)) {
        logch |= static_cast<int>(LogChannel::Perception);
    }
    if (_checkListBoxLogChannels->IsChecked(6)) {
        logch |= static_cast<int>(LogChannel::Conversation);
    }
    if (_checkListBoxLogChannels->IsChecked(7)) {
        logch |= static_cast<int>(LogChannel::Combat);
    }
    if (_checkListBoxLogChannels->IsChecked(8)) {
        logch |= static_cast<int>(LogChannel::Script);
    }
    if (_checkListBoxLogChannels->IsChecked(9)) {
        logch |= static_cast<int>(LogChannel::Script2);
    }
    if (_checkListBoxLogChannels->IsChecked(10)) {
        logch |= static_cast<int>(LogChannel::Script3);
    }

    int winScaleSel = _choiceWinScale->GetSelection();
    auto winScaleSelStr = _choiceWinScale->GetString(winScaleSel).Mid(0, 3);
    int winScale = atoi(winScaleSelStr.c_str());

    _config.gameDir = _textCtrlGameDir->GetValue();
    _config.devMode = _checkBoxDev->IsChecked();
    _config.width = stoi(tokens[0]);
    _config.height = stoi(tokens[1]);
    _config.winscale = winScale;
    _config.fullscreen = _checkBoxFullscreen->IsChecked();
    _config.vsync = _checkBoxVSync->IsChecked();
    _config.grass = _checkBoxGrass->IsChecked();
    auto rendererSel = _choiceRenderer->GetStringSelection();
    _config.mode = rendererSel == "Path tracing" ? "path-tracing"
                   : rendererSel == "PBR"        ? "pbr"
                                                 : "retro";
    _config.ptspp = wxAtoi(_choicePathTracingSamples->GetStringSelection());
    _config.ssao = _checkBoxSSAO->IsChecked();
    _config.ssr = _checkBoxSSR->IsChecked();
    switch (_choiceAntiAliasing->GetSelection()) {
    case 3: _config.antialiasing = "dlssrr"; break;
    case 2: _config.antialiasing = "fsr"; break;
    case 1: _config.antialiasing = "fxaa"; break;
    default: _config.antialiasing = "off"; break;
    }
    switch (_choiceDlssMode->GetSelection()) {
    case 4: _config.dlssMode = "ultraperformance"; break;
    case 3: _config.dlssMode = "performance"; break;
    case 2: _config.dlssMode = "balanced"; break;
    case 1: _config.dlssMode = "quality"; break;
    default: _config.dlssMode = "dlaa"; break;
    }
    _config.renderScale = _sliderRenderScale->GetValue() / 100.0f;
    _config.sharpness = _sliderSharpness->GetValue() / 100.0f;
    _config.texQuality = _choiceTextureQuality->GetSelection();
    _config.shadowres = _choiceShadowResolution->GetSelection();
    _config.anisofilter = _choiceAnisoFilter->GetSelection();
    _config.drawdist = _sliderDrawDistance->GetValue();
    _config.musicvol = _sliderVolumeMusic->GetValue();
    _config.voicevol = _sliderVolumeVoice->GetValue();
    _config.soundvol = _sliderVolumeSound->GetValue();
    _config.movievol = _sliderVolumeMovie->GetValue();
    _config.logsev = _choiceLogSeverity->GetSelection();
    _config.logch = logch;

    std::vector<std::string> lines;

    std::ifstream in(kConfigFilename);
    for (std::string line; getline(in, line);) {
        bool add = true;
        for (auto &opt : recognized) {
            if (boost::starts_with(line, opt)) {
                add = false;
                break;
            }
        }
        if (add) {
            lines.push_back(line);
        }
    }

    std::ofstream config(kConfigFilename);
    config << "game=" << _config.gameDir << std::endl;
    config << "dev=" << (_config.devMode ? 1 : 0) << std::endl;
    config << "width=" << _config.width << std::endl;
    config << "height=" << _config.height << std::endl;
    config << "winscale=" << _config.winscale << std::endl;
    config << "fullscreen=" << (_config.fullscreen ? 1 : 0) << std::endl;
    config << "vsync=" << (_config.vsync ? 1 : 0) << std::endl;
    config << "grass=" << (_config.grass ? 1 : 0) << std::endl;
    config << "mode=" << _config.mode << std::endl;
    config << "ptspp=" << _config.ptspp << std::endl;
    config << "ssao=" << (_config.ssao ? 1 : 0) << std::endl;
    config << "ssr=" << (_config.ssr ? 1 : 0) << std::endl;
    config << "antialiasing=" << _config.antialiasing << std::endl;
    config << "dlssmode=" << _config.dlssMode << std::endl;
    config << "renderscale=" << _config.renderScale << std::endl;
    config << "sharpness=" << _config.sharpness << std::endl;
    config << "texquality=" << _config.texQuality << std::endl;
    config << "shadowres=" << _config.shadowres << std::endl;
    config << "anisofilter=" << _config.anisofilter << std::endl;
    config << "drawdist=" << _config.drawdist << std::endl;
    config << "musicvol=" << _config.musicvol << std::endl;
    config << "voicevol=" << _config.voicevol << std::endl;
    config << "soundvol=" << _config.soundvol << std::endl;
    config << "movievol=" << _config.movievol << std::endl;
    config << "logsev=" << _config.logsev << std::endl;
    config << "logch=" << _config.logch << std::endl;
    for (auto &line : lines) {
        config << line << std::endl;
    }
}

void LauncherFrame::OnSaveConfig(wxCommandEvent &event) {
    SaveConfiguration();
}

void LauncherFrame::OnGameDirLeftDown(wxMouseEvent &event) {
    wxDirDialog dlg(nullptr, "Choose game directory", _textCtrlGameDir->GetValue(), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) {
        _textCtrlGameDir->SetValue(dlg.GetPath());
    }
}

} // namespace reone
