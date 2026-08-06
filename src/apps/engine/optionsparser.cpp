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
#include <stdexcept>
#include <string>

#include "reone/graphics/optionsregistry.h"
#include "reone/system/types.h"

using namespace boost::program_options;

using namespace reone::game;
using namespace reone::graphics;

namespace reone {

static constexpr char kConfigFilename[] = "reone.cfg";

/** The written form of the anti-aliasing slot, as reone.cfg stores it. */
static const char *antiAliasingName(AntiAliasing value) {
    switch (value) {
    case AntiAliasing::Fxaa:
        return "fxaa";
    case AntiAliasing::Fsr:
        return "fsr";
    default:
        return "off";
    }
}

/**
 * A misspelt mode is rejected rather than silently taken as none: the slot
 * changes what every frame looks like, and a typo that quietly disables
 * anti-aliasing is indistinguishable from the engine ignoring the flag.
 */
static AntiAliasing parseAntiAliasing(const std::string &value) {
    if (value == "off" || value == "none") {
        return AntiAliasing::None;
    }
    if (value == "fxaa") {
        return AntiAliasing::Fxaa;
    }
    if (value == "fsr") {
        return AntiAliasing::Fsr;
    }
    throw std::invalid_argument("Unknown anti-aliasing mode '" + value +
                                "'; expected off, fxaa or fsr");
}

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
        ("commands-frame-scheduled", value<std::string>()->default_value(""), "execute a second command file on commands-frame") //
        ("input-script", value<std::string>()->default_value(""), "run frame-indexed SDL mouse input script")                  //
        ("capture", value<std::string>()->default_value(""), "write a screenshot to this path and exit")                        //
        ("dumptargets", value<std::string>()->default_value(""), "write the scene render targets to this directory as .npy")   //
        ("dumpobjects", value<std::string>()->default_value(""), "append the module's traced-emissive candidates to this file") //
        ("captureframe", value<int>()->default_value(3), "frame to capture on, counted from the first rendered frame")         //
        ("captureframes", value<int>()->default_value(1), "capture this many consecutive frames, numbered into the filename") //
        ("freezeframe", value<int>()->default_value(0), "stop advancing the simulation from this frame on, or 0 not to")      //
        ("randomseed", value<int>()->default_value(-1), "seed the random generator, or -1 to seed from the clock")             //
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
        ("grassdensity", value<float>()->default_value(options->graphics.grassDensity), "grass density multiplier")           //
        ("mode", value<std::string>()->default_value(renderModeName(options->graphics.mode)), "render mode: retro, pbr or path-tracing ('raster' is accepted as a spelling of retro)") //
        ("admissionshadow", value<bool>()->default_value(false), "compare incremental and full scene admission every frame") //
        ("admissionforcefull", value<bool>()->default_value(false), "force full scene collection and classification")        //
        ("ptspp", value<int>()->default_value(options->graphics.pathTracingSamples), "path tracing samples per pixel")          //
        ("skyintensity", value<float>()->default_value(options->graphics.skyIntensity), "sky light intensity")                //
        ("ptemissiveintensity", value<float>()->default_value(options->graphics.ptEmissiveIntensity), "path tracing emissive intensity") //
        ("ptlightmapintensity", value<float>()->default_value(options->graphics.ptLightmapIntensity), "path tracing lightmap intensity") //
        ("ptdirectintensity", value<float>()->default_value(options->graphics.ptDirectIntensity), "path tracing direct-light intensity") //
        ("ptsunintensity", value<float>()->default_value(options->graphics.ptSunIntensity), "path tracing sun intensity")       //
        ("ptbounces", value<int>()->default_value(options->graphics.ptBounces), "path tracing bounces")                       //
        ("ptrayoffset", value<float>()->default_value(options->graphics.ptRayOffset), "path tracing ray origin offset")        //
        ("pttracestats", value<bool>()->default_value(options->graphics.ptTraceStats), "enable path tracing statistics")       //
        ("tonemap", value<int>()->default_value(options->graphics.tonemap), "display transform: 0 off, 1 Gran Turismo curve")               //
        ("exposure", value<float>()->default_value(options->graphics.exposure), "scene-referred exposure ahead of the tonemap") //
        ("ptpointemitterratio", value<float>()->default_value(options->graphics.ptPointEmitterRatio), "path tracing point-light emitter radius, as a fraction of influence radius") //
        ("ptbounceroughness", value<float>()->default_value(options->graphics.ptBounceRoughness), "roughness floor after the first scatter (path regularisation)") //
        ("ptroughnessfloor", value<float>()->default_value(options->graphics.ptRoughnessFloor), "lowest roughness any surface may take") //
        ("ptindirectclamp", value<float>()->default_value(options->graphics.ptIndirectClamp), "ceiling on one indirect sample, 0 to disable") //
        ("ptsunangularsize", value<float>()->default_value(options->graphics.ptSunAngularSize), "path tracing sun angular size") //
        ("lightmaps", value<bool>()->default_value(options->graphics.lightmaps), "apply lightmaps (diagnostic toggle)")        //
        ("ssao", value<bool>()->default_value(options->graphics.ssao), "enable screen-space ambient occlusion")                 //
        ("ssr", value<bool>()->default_value(options->graphics.ssr), "enable screen-space reflections")                         //
        ("antialiasing", value<std::string>()->default_value(antiAliasingName(options->graphics.antialiasing)),
         "anti-aliasing in the common slot: off, fxaa or fsr; defaults per render mode")                                       //
        ("grade", value<bool>()->default_value(options->graphics.grade),
         "apply exposure and the tone curve; the display transform itself always runs")                                        //
        ("post", value<bool>()->default_value(options->graphics.grade),
         "deprecated alias for --grade")                                                                                       //
        ("sharpen", value<bool>()->default_value(options->graphics.sharpen), "sharpen the finished frame (unsharp mask, after the display transform)") //
        ("sharpenamount", value<float>()->default_value(options->graphics.sharpenAmount), "strength of that mask")       //
        ("ptdenoise", value<bool>()->default_value(options->graphics.ptDenoise), "enable the path tracing denoiser")           //
        ("debugview", value<int>()->default_value(options->graphics.debugView),
         "debug channel view in any render mode, 0 off")                                                                  //
        ("ptdebugview", value<int>()->default_value(options->graphics.debugView),
         "deprecated alias for --debugview")                                                                              //
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
        ("fsrsharpness", value<float>()->default_value(options->graphics.fsrSharpness),
         "FSR RCAS sharpening, 0 disables the pass")                                                                          //
        ("texquality", value<int>()->default_value(static_cast<int>(options->graphics.textureQuality)), "texture quality")      //
        ("shadowres", value<int>()->default_value(glm::log2(options->graphics.shadowResolution) - 10), "shadow map resolution") //
        ("shadowopacity", value<float>()->default_value(options->graphics.shadowOpacity), "override the module's authored shadow opacity (0-1; <0 keeps authored)") //
        ("anisofilter", value<int>()->default_value(options->graphics.anisotropicFiltering), "anisotropic filtering")           //
        ("drawdist", value<float>()->default_value(options->graphics.drawDistance), "draw distance")                           //
        ("musicvol", value<int>()->default_value(options->audio.musicVolume), "music volume in percents")                       //
        ("voicevol", value<int>()->default_value(options->audio.voiceVolume), "voice volume in percents")                       //
        ("soundvol", value<int>()->default_value(options->audio.soundVolume), "sound volume in percents")                       //
        ("movievol", value<int>()->default_value(options->audio.movieVolume), "movie volume in percents")                       //
        ("logsev", value<int>()->default_value(static_cast<int>(options->logging.severity)), "minimum log severity")            //
        ("logch", value<int>()->default_value(defaultLogChannels), "log channel mask");

    for (int i = 0; i < 9; ++i) {
        auto &override = options->graphics.categoryOverrides[i];
        auto key = "cat" + std::to_string(i);
        descCommon.add_options()                                                                                               //
            ((key + "color0").c_str(), value<float>()->default_value(override.color[0]), "material category color red")   //
            ((key + "color1").c_str(), value<float>()->default_value(override.color[1]), "material category color green") //
            ((key + "color2").c_str(), value<float>()->default_value(override.color[2]), "material category color blue")  //
            ((key + "colorweight").c_str(), value<float>()->default_value(override.colorWeight), "material category color weight") //
            ((key + "roughness").c_str(), value<float>()->default_value(override.roughness), "material category roughness") //
            ((key + "roughnessscale").c_str(), value<float>()->default_value(override.roughnessScale), "material category roughness scale") //
            ((key + "emission").c_str(), value<float>()->default_value(override.emissionScale), "material category emission scale") //
            ((key + "env").c_str(), value<float>()->default_value(override.envScale), "material category environment scale") //
            ((key + "metallic").c_str(), value<float>()->default_value(override.metallicScale), "material category metallic scale");
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
    // The per-frame upload hash exists for the dump log line; do not pay for
    // it on frames nobody will ever compare.
    options->graphics.hashUploads = !options->dumpTargetsPath.empty();
    options->dumpObjectsPath = vars["dumpobjects"].as<std::string>();
    options->captureFrame = vars["captureframe"].as<int>();
    options->captureFrames = std::max(1, vars["captureframes"].as<int>());
    options->freezeFrame = vars["freezeframe"].as<int>();
    options->randomSeed = vars["randomseed"].as<int>();
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
    options->graphics.grassDensity = vars["grassdensity"].as<float>();
    options->graphics.mode = parseRenderMode(vars["mode"].as<std::string>());
    options->graphics.admissionShadow = vars["admissionshadow"].as<bool>();
    options->graphics.admissionForceFull = vars["admissionforcefull"].as<bool>();
    options->graphics.pathTracingSamples = std::max(1, vars["ptspp"].as<int>());
    options->graphics.skyIntensity = vars["skyintensity"].as<float>();
    options->graphics.ptEmissiveIntensity = vars["ptemissiveintensity"].as<float>();
    options->graphics.ptLightmapIntensity = vars["ptlightmapintensity"].as<float>();
    options->graphics.ptDirectIntensity = vars["ptdirectintensity"].as<float>();
    options->graphics.ptSunIntensity = vars["ptsunintensity"].as<float>();
    options->graphics.ptBounceRoughness = std::clamp(vars["ptbounceroughness"].as<float>(), 0.0f, 1.0f);
    options->graphics.ptRoughnessFloor = std::clamp(vars["ptroughnessfloor"].as<float>(), 0.0f, 1.0f);
    options->graphics.ptIndirectClamp = std::max(0.0f, vars["ptindirectclamp"].as<float>());
    options->graphics.ptBounces = std::clamp(vars["ptbounces"].as<int>(), 1, 8);
    options->graphics.ptRayOffset = std::max(0.0001f, vars["ptrayoffset"].as<float>());
    options->graphics.ptTraceStats = vars["pttracestats"].as<bool>();
    options->graphics.tonemap = std::clamp(vars["tonemap"].as<int>(), 0, 1);
    options->graphics.exposure = std::max(0.05f, vars["exposure"].as<float>());
    options->graphics.ptPointEmitterRatio = std::clamp(vars["ptpointemitterratio"].as<float>(), 0.01f, 0.5f);
    options->graphics.ptSunAngularSize = std::max(0.05f, vars["ptsunangularsize"].as<float>());
    options->graphics.lightmaps = vars["lightmaps"].as<bool>();
    options->graphics.ssao = vars["ssao"].as<bool>();
    options->graphics.ssr = vars["ssr"].as<bool>();
    // Resolved here, where the render mode is also known, so that nothing
    // deeper has to ask again: below this point the option says what the slot
    // runs, full stop. A traced frame is noisy and carries the motion a
    // temporal resolve wants, so it takes FSR; raster keeps the cheap spatial
    // filter it has always had. An explicit flag always wins.
    if (vars["antialiasing"].defaulted()) {
        options->graphics.antialiasing = options->graphics.mode == RenderMode::PathTracing
                                             ? AntiAliasing::Fsr
                                             : AntiAliasing::Fxaa;
    } else {
        options->graphics.antialiasing =
            parseAntiAliasing(vars["antialiasing"].as<std::string>());
    }
    // The dial the post-process pass reads is `grade`: that pass is no longer
    // optional - it is the frame's only encode - so what is switchable is the
    // exposure and the tone curve it applies. `post` is the name that dial had
    // when it did switch the pass itself, kept working here rather than broken
    // for every script and commands file already written against it. An
    // explicit --grade wins; otherwise --post is read, and it defaults to the
    // same value, so a run that passes neither is unaffected.
    options->graphics.grade = vars["grade"].defaulted() ? vars["post"].as<bool>()
                                                        : vars["grade"].as<bool>();
    options->graphics.sharpen = vars["sharpen"].as<bool>();
    options->graphics.sharpenAmount = std::max(0.0f, vars["sharpenamount"].as<float>());
    options->graphics.ptDenoise = vars["ptdenoise"].as<bool>();
    // Same shape as --grade / --post above: the dial dropped its pt prefix when
    // the views stopped being tracer-only, and the old spelling keeps working
    // rather than breaking every script already written against it. An explicit
    // --debugview wins; otherwise --ptdebugview is read, and both default to
    // the same value, so a run that passes neither is unaffected.
    options->graphics.debugView =
        std::clamp(vars["debugview"].defaulted() ? vars["ptdebugview"].as<int>()
                                                 : vars["debugview"].as<int>(),
                   0, 14);
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
    options->graphics.fsrSharpness = std::clamp(vars["fsrsharpness"].as<float>(), 0.0f, 1.0f);
    options->graphics.textureQuality = static_cast<TextureQuality>(vars["texquality"].as<int>());
    options->graphics.shadowResolution = 1 << (10 + vars["shadowres"].as<int>());
    options->graphics.shadowOpacity = vars["shadowopacity"].as<float>();
    options->graphics.anisotropicFiltering = vars["anisofilter"].as<int>();
    options->graphics.drawDistance = vars["drawdist"].as<float>();
    for (int i = 0; i < 9; ++i) {
        auto &override = options->graphics.categoryOverrides[i];
        auto key = "cat" + std::to_string(i);
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
    options->commandsFrameScheduledFile = vars["commands-frame-scheduled"].as<std::string>();
    options->inputScript = vars["input-script"].as<std::string>();

    return options;
}

} // namespace reone
