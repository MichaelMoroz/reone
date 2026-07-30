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

#include "optionsparser.h"

#include <algorithm>

#include "reone/system/types.h"

using namespace boost::program_options;

using namespace reone::game;
using namespace reone::graphics;

namespace reone {

static constexpr char kConfigFilename[] = "reone.cfg";

std::unique_ptr<Options> OptionsParser::parse() {
    auto options = std::make_unique<Options>();

    int defaultLogChannels = 0;
    for (auto &channel : options->logging.channels) {
        defaultLogChannels |= static_cast<int>(channel);
    }

    // Initialize options description

    options_description descCommon;
    descCommon.add_options()                                                                                                    //
        ("game", value<std::string>(), "path to game directory")                                                                //
        ("commands-file", value<std::string>()->default_value(""), "execute console commands from a file at startup")           //
        ("commands-frame", value<int>()->default_value(0), "run the commands file on this frame instead of at startup")       //
        ("input-script", value<std::string>()->default_value(""), "run frame-indexed SDL mouse input script")                  //
        ("capture", value<std::string>()->default_value(""), "write a screenshot to this path and exit")                        //
        ("dumptargets", value<std::string>()->default_value(""), "write the scene render targets to this directory as .npy")   //
        ("dumpobjects", value<std::string>()->default_value(""), "append the module's traced-emissive candidates to this file") //
        ("captureframe", value<int>()->default_value(3), "frame to capture on, counted from the first rendered frame")         //
        ("captureframes", value<int>()->default_value(1), "capture this many consecutive frames, numbered into the filename") //
        ("freezeframe", value<int>()->default_value(0), "stop advancing the simulation from this frame on, or 0 not to")      //
        ("randomseed", value<int>()->default_value(-1), "seed the random generator, or -1 to seed from the clock")             //
        ("backend", value<std::string>()->default_value("gl"), "graphics backend: gl or vulkan")                              //
        ("vkvalidation", value<bool>()->default_value(false), "enable Vulkan validation layers")                              //
        ("renderdoc", value<bool>()->default_value(false), "trigger a RenderDoc frame capture with the screenshot")            //
        ("dev", value<bool>()->default_value(options->game.developer), "enable developer mode")                                 //
        ("width", value<int>()->default_value(options->graphics.width), "render width")                                         //
        ("height", value<int>()->default_value(options->graphics.height), "render height")                                      //
        ("winscale", value<int>()->default_value(options->graphics.winScale), "window scale")                                   //
        ("fullscreen", value<bool>()->default_value(options->graphics.fullscreen), "enable fullscreen")                         //
        ("headless", value<bool>()->default_value(false), "never show the window; for scripted batch runs")                     //
        ("vsync", value<bool>()->default_value(options->graphics.vsync), "enable v-sync")                                       //
        ("grass", value<bool>()->default_value(options->graphics.grass), "enable grass")                                        //
        ("pbr", value<bool>()->default_value(options->graphics.pbr), "enable physically-based rendering")                       //
        ("mode", value<std::string>()->default_value(options->graphics.mode), "render mode: raster or path-tracing")            //
        ("ptspp", value<int>()->default_value(options->graphics.pathTracingSamples), "path tracing samples per pixel")          //
        ("ptskyintensity", value<float>()->default_value(options->graphics.ptSkyIntensity), "path tracing sky intensity")       //
        ("ptemissiveintensity", value<float>()->default_value(options->graphics.ptEmissiveIntensity), "path tracing emissive intensity") //
        ("ptlightmapintensity", value<float>()->default_value(options->graphics.ptLightmapIntensity), "path tracing lightmap intensity") //
        ("ptdirectintensity", value<float>()->default_value(options->graphics.ptDirectIntensity), "path tracing direct-light intensity") //
        ("ptsunintensity", value<float>()->default_value(options->graphics.ptSunIntensity), "path tracing sun intensity")       //
        ("ptbounces", value<int>()->default_value(options->graphics.ptBounces), "path tracing bounces")                       //
        ("ptrayoffset", value<float>()->default_value(options->graphics.ptRayOffset), "path tracing ray origin offset")        //
        ("pttracestats", value<bool>()->default_value(options->graphics.ptTraceStats), "enable path tracing statistics")       //
        ("pttonemap", value<int>()->default_value(options->graphics.ptTonemap), "path tracing display transform")              //
        ("ptexposure", value<float>()->default_value(options->graphics.ptExposure), "path tracing exposure")                   //
        ("ptpointangularsize", value<float>()->default_value(options->graphics.ptPointAngularSize), "path tracing point-light angular size") //
        ("ptsunangularsize", value<float>()->default_value(options->graphics.ptSunAngularSize), "path tracing sun angular size") //
        ("ssao", value<bool>()->default_value(options->graphics.ssao), "enable screen-space ambient occlusion")                 //
        ("ssr", value<bool>()->default_value(options->graphics.ssr), "enable screen-space reflections")                         //
        ("fxaa", value<bool>()->default_value(options->graphics.fxaa), "enable anti-aliasing")                                  //
        ("sharpen", value<bool>()->default_value(options->graphics.sharpen), "enable image sharpening")                         //
        ("taajitter", value<bool>()->default_value(options->graphics.taaJitter), "enable sub-pixel projection jitter")          //
        ("ptdenoise", value<bool>()->default_value(options->graphics.ptDenoise), "enable the path tracing denoiser")           //
        ("ptdebugview", value<int>()->default_value(options->graphics.ptDebugView),
         "path tracing debug view, 0 off")                                                                                //
        ("ptnrdstabilized", value<int>()->default_value(options->graphics.ptNrdMaxStabilizedFrames),
         "REBLUR stabilized frames; 0 disables its temporal stabilization pass")                                              //
        ("ptnrdaccum", value<int>()->default_value(options->graphics.ptNrdMaxAccumulatedFrames),
         "REBLUR accumulated frames, up to 63")                                                                               //
        ("ptnrdfastaccum", value<int>()->default_value(options->graphics.ptNrdMaxFastAccumulatedFrames),
         "REBLUR fast accumulated frames")                                                                                    //
        ("ptnrdhistoryfix", value<int>()->default_value(options->graphics.ptNrdHistoryFixFrames),
         "REBLUR history fix frames")                                                                                         //
        ("ptnrddiffuseprepassblurradius", value<float>()->default_value(options->graphics.ptNrdDiffusePrepassBlurRadius),
         "REBLUR diffuse prepass blur radius")                                                                                //
        ("ptnrdspecularprepassblurradius", value<float>()->default_value(options->graphics.ptNrdSpecularPrepassBlurRadius),
         "REBLUR specular prepass blur radius")                                                                               //
        ("ptnrdminblurradius", value<float>()->default_value(options->graphics.ptNrdMinBlurRadius),
         "REBLUR minimum blur radius")                                                                                        //
        ("ptnrdmaxblurradius", value<float>()->default_value(options->graphics.ptNrdMaxBlurRadius),
         "REBLUR maximum blur radius")                                                                                        //
        ("ptnrdlobeanglefraction", value<float>()->default_value(options->graphics.ptNrdLobeAngleFraction),
         "REBLUR lobe angle fraction")                                                                                        //
        ("ptnrdroughnessfraction", value<float>()->default_value(options->graphics.ptNrdRoughnessFraction),
         "REBLUR roughness fraction")                                                                                         //
        ("ptnrdplanedistancesensitivity", value<float>()->default_value(options->graphics.ptNrdPlaneDistanceSensitivity),
         "REBLUR plane distance sensitivity")                                                                                 //
        ("ptnrddisocclusionthreshold", value<float>()->default_value(options->graphics.ptNrdDisocclusionThreshold),
         "REBLUR disocclusion threshold")                                                                                     //
        ("ptnrdantifirefly", value<bool>()->default_value(options->graphics.ptNrdAntiFirefly),
         "enable REBLUR anti-firefly")                                                                                        //
        ("ptfsr", value<bool>()->default_value(options->graphics.ptFsr),
         "anti-alias with FidelityFX Super Resolution at NativeAA")                                           //
        ("ptfsrsharpness", value<float>()->default_value(options->graphics.ptFsrSharpness),
         "FSR RCAS sharpening, 0 disables the pass")                                                                          //
        ("texquality", value<int>()->default_value(static_cast<int>(options->graphics.textureQuality)), "texture quality")      //
        ("shadowres", value<int>()->default_value(glm::log2(options->graphics.shadowResolution) - 10), "shadow map resolution") //
        ("anisofilter", value<int>()->default_value(options->graphics.anisotropicFiltering), "anisotropic filtering")           //
        ("drawdist", value<float>()->default_value(options->graphics.drawDistance), "draw distance")                           //
        ("musicvol", value<int>()->default_value(options->audio.musicVolume), "music volume in percents")                       //
        ("voicevol", value<int>()->default_value(options->audio.voiceVolume), "voice volume in percents")                       //
        ("soundvol", value<int>()->default_value(options->audio.soundVolume), "sound volume in percents")                       //
        ("movievol", value<int>()->default_value(options->audio.movieVolume), "movie volume in percents")                       //
        ("logsev", value<int>()->default_value(static_cast<int>(options->logging.severity)), "minimum log severity")            //
        ("logch", value<int>()->default_value(defaultLogChannels), "log channel mask");

    for (int i = 0; i < 9; ++i) {
        auto &override = options->graphics.ptCategoryOverrides[i];
        auto key = "ptcat" + std::to_string(i);
        descCommon.add_options()                                                                                               //
            ((key + "color0").c_str(), value<float>()->default_value(override.color[0]), "path tracing category color red")   //
            ((key + "color1").c_str(), value<float>()->default_value(override.color[1]), "path tracing category color green") //
            ((key + "color2").c_str(), value<float>()->default_value(override.color[2]), "path tracing category color blue")  //
            ((key + "colorweight").c_str(), value<float>()->default_value(override.colorWeight), "path tracing category color weight") //
            ((key + "roughness").c_str(), value<float>()->default_value(override.roughness), "path tracing category roughness") //
            ((key + "roughnessscale").c_str(), value<float>()->default_value(override.roughnessScale), "path tracing category roughness scale") //
            ((key + "emission").c_str(), value<float>()->default_value(override.emissionScale), "path tracing category emission scale") //
            ((key + "env").c_str(), value<float>()->default_value(override.envScale), "path tracing category environment scale") //
            ((key + "metallic").c_str(), value<float>()->default_value(override.metallicScale), "path tracing category metallic scale");
    }

    options_description descCmdLine {"Usage"};
    descCmdLine.add(descCommon);

    // Parse command line and configuration file

    variables_map vars;
    store(parse_command_line(_argc, _argv, descCmdLine), vars);
    if (std::filesystem::exists(kConfigFilename)) {
        store(parse_config_file<char>(kConfigFilename, descCommon, true), vars);
    }
    notify(vars);

    // Convert Boost options to game options

    options->game.path = vars.count("game") > 0 ? std::filesystem::path(vars["game"].as<std::string>()) : std::filesystem::current_path();
    options->game.developer = vars["dev"].as<bool>();
    options->capturePath = vars["capture"].as<std::string>();
    options->dumpTargetsPath = vars["dumptargets"].as<std::string>();
    options->dumpObjectsPath = vars["dumpobjects"].as<std::string>();
    options->captureFrame = vars["captureframe"].as<int>();
    options->captureFrames = std::max(1, vars["captureframes"].as<int>());
    options->freezeFrame = vars["freezeframe"].as<int>();
    options->randomSeed = vars["randomseed"].as<int>();
    options->backend = vars["backend"].as<std::string>();
    options->vulkanValidation = vars["vkvalidation"].as<bool>();
    options->renderdoc = vars["renderdoc"].as<bool>();
    options->graphics.headless = vars["headless"].as<bool>();
    options->audio.muted = options->graphics.headless;
    options->graphics.width = vars["width"].as<int>();
    options->graphics.height = vars["height"].as<int>();
    options->graphics.winScale = vars["winscale"].as<int>();
    options->graphics.fullscreen = vars["fullscreen"].as<bool>();
    options->graphics.vsync = vars["vsync"].as<bool>();
    options->graphics.grass = vars["grass"].as<bool>();
    options->graphics.pbr = vars["pbr"].as<bool>();
    options->graphics.mode = vars["mode"].as<std::string>();
    options->graphics.pathTracingSamples = std::max(1, vars["ptspp"].as<int>());
    options->graphics.ptSkyIntensity = vars["ptskyintensity"].as<float>();
    options->graphics.ptEmissiveIntensity = vars["ptemissiveintensity"].as<float>();
    options->graphics.ptLightmapIntensity = vars["ptlightmapintensity"].as<float>();
    options->graphics.ptDirectIntensity = vars["ptdirectintensity"].as<float>();
    options->graphics.ptSunIntensity = vars["ptsunintensity"].as<float>();
    options->graphics.ptBounces = std::clamp(vars["ptbounces"].as<int>(), 1, 8);
    options->graphics.ptRayOffset = std::max(0.0001f, vars["ptrayoffset"].as<float>());
    options->graphics.ptTraceStats = vars["pttracestats"].as<bool>();
    options->graphics.ptTonemap = std::clamp(vars["pttonemap"].as<int>(), 0, 1);
    options->graphics.ptExposure = std::max(0.05f, vars["ptexposure"].as<float>());
    options->graphics.ptPointAngularSize = std::max(0.05f, vars["ptpointangularsize"].as<float>());
    options->graphics.ptSunAngularSize = std::max(0.05f, vars["ptsunangularsize"].as<float>());
    options->graphics.ssao = vars["ssao"].as<bool>();
    options->graphics.ssr = vars["ssr"].as<bool>();
    options->graphics.fxaa = vars["fxaa"].as<bool>();
    options->graphics.sharpen = vars["sharpen"].as<bool>();
    options->graphics.taaJitter = vars["taajitter"].as<bool>();
    options->graphics.ptDenoise = vars["ptdenoise"].as<bool>();
    options->graphics.ptDebugView = std::clamp(vars["ptdebugview"].as<int>(), 0, 12);
    options->graphics.ptNrdMaxStabilizedFrames = std::max(0, vars["ptnrdstabilized"].as<int>());
    options->graphics.ptNrdMaxAccumulatedFrames = std::clamp(vars["ptnrdaccum"].as<int>(), 0, 63);
    options->graphics.ptNrdMaxFastAccumulatedFrames = std::max(0, vars["ptnrdfastaccum"].as<int>());
    options->graphics.ptNrdHistoryFixFrames = std::max(0, vars["ptnrdhistoryfix"].as<int>());
    options->graphics.ptNrdDiffusePrepassBlurRadius = std::max(0.0f, vars["ptnrddiffuseprepassblurradius"].as<float>());
    options->graphics.ptNrdSpecularPrepassBlurRadius = std::max(0.0f, vars["ptnrdspecularprepassblurradius"].as<float>());
    options->graphics.ptNrdMinBlurRadius = std::max(0.0f, vars["ptnrdminblurradius"].as<float>());
    options->graphics.ptNrdMaxBlurRadius = std::max(0.0f, vars["ptnrdmaxblurradius"].as<float>());
    options->graphics.ptNrdLobeAngleFraction = std::clamp(vars["ptnrdlobeanglefraction"].as<float>(), 0.01f, 1.0f);
    options->graphics.ptNrdRoughnessFraction = std::clamp(vars["ptnrdroughnessfraction"].as<float>(), 0.01f, 1.0f);
    options->graphics.ptNrdPlaneDistanceSensitivity = std::max(0.0f, vars["ptnrdplanedistancesensitivity"].as<float>());
    options->graphics.ptNrdDisocclusionThreshold = std::max(0.0f, vars["ptnrddisocclusionthreshold"].as<float>());
    options->graphics.ptNrdAntiFirefly = vars["ptnrdantifirefly"].as<bool>();
    options->graphics.ptFsr = vars["ptfsr"].as<bool>();
    options->graphics.ptFsrSharpness = std::clamp(vars["ptfsrsharpness"].as<float>(), 0.0f, 1.0f);
#ifdef R_ENABLE_NRD
    // Jitter feeds NRD's temporal accumulation, and the tracer picks it up
    // through the jittered projection for free. On by default for path
    // tracing in NRD builds - deliberately, it changes every screenshot -
    // while an explicit --taajitter flag still wins.
    if (vars["mode"].as<std::string>() == "path-tracing" && vars["taajitter"].defaulted()) {
        options->graphics.taaJitter = true;
    }
#endif
    options->graphics.textureQuality = static_cast<TextureQuality>(vars["texquality"].as<int>());
    options->graphics.shadowResolution = 1 << (10 + vars["shadowres"].as<int>());
    options->graphics.anisotropicFiltering = vars["anisofilter"].as<int>();
    options->graphics.drawDistance = vars["drawdist"].as<float>();
    for (int i = 0; i < 9; ++i) {
        auto &override = options->graphics.ptCategoryOverrides[i];
        auto key = "ptcat" + std::to_string(i);
        override.color[0] = vars[key + "color0"].as<float>();
        override.color[1] = vars[key + "color1"].as<float>();
        override.color[2] = vars[key + "color2"].as<float>();
        override.colorWeight = vars[key + "colorweight"].as<float>();
        override.roughness = vars[key + "roughness"].as<float>();
        override.roughnessScale = std::max(0.0f, vars[key + "roughnessscale"].as<float>());
        override.emissionScale = vars[key + "emission"].as<float>();
        override.envScale = vars[key + "env"].as<float>();
        override.metallicScale = vars[key + "metallic"].as<float>();
    }
    options->audio.musicVolume = vars["musicvol"].as<int>();
    options->audio.voiceVolume = vars["voicevol"].as<int>();
    options->audio.soundVolume = vars["soundvol"].as<int>();
    options->audio.movieVolume = vars["movievol"].as<int>();
    options->logging.severity = static_cast<LogSeverity>(vars["logsev"].as<int>());

    std::set<LogChannel> logChannels;
    int logChannelsMask = vars["logch"].as<int>();
    if ((logChannelsMask & static_cast<int>(LogChannel::Global)) != 0) {
        logChannels.insert(LogChannel::Global);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::Resources)) != 0) {
        logChannels.insert(LogChannel::Resources);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::Resources2)) != 0) {
        logChannels.insert(LogChannel::Resources2);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::Graphics)) != 0) {
        logChannels.insert(LogChannel::Graphics);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::Audio)) != 0) {
        logChannels.insert(LogChannel::Audio);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::GUI)) != 0) {
        logChannels.insert(LogChannel::GUI);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::Perception)) != 0) {
        logChannels.insert(LogChannel::Perception);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::Conversation)) != 0) {
        logChannels.insert(LogChannel::Conversation);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::Combat)) != 0) {
        logChannels.insert(LogChannel::Combat);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::Script)) != 0) {
        logChannels.insert(LogChannel::Script);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::Script2)) != 0) {
        logChannels.insert(LogChannel::Script2);
    }
    if ((logChannelsMask & static_cast<int>(LogChannel::Script3)) != 0) {
        logChannels.insert(LogChannel::Script3);
    }
    options->logging.channels = std::move(logChannels);

    options->commandsFile = vars["commands-file"].as<std::string>();
    options->commandsFrame = vars["commands-frame"].as<int>();
    options->inputScript = vars["input-script"].as<std::string>();

    return options;
}

} // namespace reone
