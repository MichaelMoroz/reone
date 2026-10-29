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
#include <string>

#include "reone/graphics/optionsregistry.h"
#include "reone/system/logutil.h"
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
        ("game", value<std::string>(), "path to game directory") //
        ("commands-file", value<std::string>()->default_value(""), "execute console commands from a file at startup") //
        ("commands-frame", value<int>()->default_value(0), "run the commands file on this frame instead of at startup") //
        ("commands-frame-scheduled", value<std::string>()->default_value(""), "execute a second command file on commands-frame") //
        ("input-script", value<std::string>()->default_value(""), "run frame-indexed SDL mouse input script") //
        ("record-input", value<std::string>()->default_value(""), "record this session's input for later replay") //
        ("dumptargets", value<std::string>()->default_value(""), "write the scene render targets to this directory as .npy") //
        ("dumpobjects", value<std::string>()->default_value(""), "append the module's traced-emissive candidates to this file") //
        ("randomseed", value<int>()->default_value(-1), "seed the random generator, or -1 to seed from the clock") //
        ("vkvalidation", value<bool>()->default_value(false), "enable Vulkan validation layers") //
        ("vkdebuglabels", value<bool>()->default_value(false), "label Vulkan passes for GPU profilers; not with capture") //
        ("renderdoc", value<bool>()->default_value(false), "trigger a RenderDoc frame capture with the screenshot") //
        ("dev", value<bool>()->default_value(options->game.developer), "enable developer mode") //
        ("skyintensity", value<float>(), "deprecated: use ptskyintensity / pbrskyintensity") //
        ("emissiveintensity", value<float>(), "deprecated: use ptemissiveintensity / pbremissiveintensity") //
        ("lightmapintensity", value<float>(), "deprecated: use ptlightmapintensity / pbrlightmapintensity") //
        ("ptskyintensity", value<float>()->default_value(-1.0f), "retired: maps to skyboxintensity") //
        ("ptemissiveintensity", value<float>()->default_value(-1.0f), "retired: maps to every category's emission grade") //
        ("ptbackdropintensity", value<float>()->default_value(-1.0f), "retired: maps to the sky room's emission grade") //
        ("emissivegamma", value<float>()->default_value(-1.0f), "retired: maps to every emission grade's gamma") //
        ("post", value<bool>()->default_value(options->graphics.grade),
         "deprecated alias for --grade") //
        ("ptdebugview", value<int>()->default_value(options->graphics.debugView),
         "deprecated alias for --debugview") //
        ("musicvol", value<int>()->default_value(options->audio.musicVolume), "music volume in percents") //
        ("voicevol", value<int>()->default_value(options->audio.voiceVolume), "voice volume in percents") //
        ("soundvol", value<int>()->default_value(options->audio.soundVolume), "sound volume in percents") //
        ("movievol", value<int>()->default_value(options->audio.movieVolume), "movie volume in percents") //
        ("logsev", value<int>()->default_value(static_cast<int>(options->logging.severity)), "minimum log severity") //
        ("logch", value<int>()->default_value(defaultLogChannels), "log channel mask");

    // Every graphics option is declared once, in optionsregistry.cpp, and
    // reaches both the command line and reone.cfg from there. The written form
    // is the same one the console and the settings file use, so a descriptor's
    // own get()/set() pair does the parsing and the clamping.
    for (const auto &desc : graphicsOptionDescs()) {
        descCommon.add_options()(desc.name.c_str(),
                                 value<std::string>()->default_value(desc.get(options->graphics)),
                                 desc.help.c_str());
    }
    // Retired spellings, kept readable so an older reone.cfg still carries its
    // grade. They have no descriptor because nothing should write them again.
    const auto addRetired = [&](const std::string &key) {
        descCommon.add_options()                                                                //
            ((key + "color0").c_str(), value<float>()->default_value(-1.0f), "retired")         //
            ((key + "color1").c_str(), value<float>()->default_value(-1.0f), "retired")         //
            ((key + "color2").c_str(), value<float>()->default_value(-1.0f), "retired")         //
            ((key + "colorweight").c_str(), value<float>()->default_value(-1.0f), "retired")    //
            ((key + "roughness").c_str(), value<float>()->default_value(-2.0f), "retired")      //
            ((key + "roughnessscale").c_str(), value<float>()->default_value(-1.0f), "retired") //
            ((key + "metallic").c_str(), value<float>()->default_value(-1.0f), "retired")       //
            ((key + "emission").c_str(), value<float>()->default_value(-1.0f), "retired");
    };
    for (int i = 0; i < 9; ++i) {
        const auto key = "cat" + std::to_string(i);
        // The reflective class kept four of its own retired keys on top of the
        // single-class set below.
        descCommon.add_options()                                                                       //
            ((key + "reflroughness").c_str(), value<float>()->default_value(-2.0f),
             "retired: maps to both roughness ends")                                                   //
            ((key + "reflroughnessscale").c_str(), value<float>()->default_value(-1.0f), "retired")  //
            ((key + "reflmetallicscale").c_str(), value<float>()->default_value(-1.0f), "retired")   //
            ((key + "reflf0").c_str(), value<float>()->default_value(-1.0f), "retired");
        addRetired(key);
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

    // Graphics options apply through their own descriptors, which parse the
    // written form and clamp it to the range the console and the settings
    // window use. Anything below this loop either predates a descriptor or
    // needs more than one key to decide its value.
    for (const auto &desc : graphicsOptionDescs()) {
        desc.set(options->graphics, vars[desc.name].as<std::string>());
    }

    options->game.path = vars.count("game") > 0 ? std::filesystem::path(vars["game"].as<std::string>()) : std::filesystem::current_path();
    options->game.developer = vars["dev"].as<bool>();
    options->dumpTargetsPath = vars["dumptargets"].as<std::string>();
    // The per-frame upload hash exists for the dump log line; do not pay for
    // it on frames nobody will ever compare.
    options->graphics.hashUploads = !options->dumpTargetsPath.empty();
    options->dumpObjectsPath = vars["dumpobjects"].as<std::string>();
    options->randomSeed = vars["randomseed"].as<int>();
    options->vulkanValidation = vars["vkvalidation"].as<bool>();
    options->vulkanDebugLabels = vars["vkdebuglabels"].as<bool>();
    options->renderdoc = vars["renderdoc"].as<bool>();
    options->audio.muted = options->graphics.headless;
    options->game.configurationPath = std::filesystem::absolute(kConfigFilename);
    options->game.menuPresentation = game::MenuPresentation::load(options->game.configurationPath);
    // A dial per mode, read straight through. There is no per-mode default
    // resolution here any more: the two grades are authored against different
    // transport, so a shared number made every adjustment a question of which
    // mode happened to be running.
    // The three retired spellings were ONE dial across both modes. A cfg
    // written against them carries a grade that would otherwise vanish
    // silently, so each still applies - but only to a mode that was not given
    // its own value, because an explicit mode-scoped dial is the more specific
    // statement of intent.
    const auto retiredDial = [&vars, &options](const char *retired, const char *ptName,
                                               const char *pbrName,
                                               float GraphicsOptions::*ptMember,
                                               float GraphicsOptions::*pbrMember) {
        if (!vars.count(retired)) {
            return;
        }
        warn(std::string(retired) + " is deprecated; use " + ptName + " and " + pbrName);
        const float value = std::max(0.0f, vars[retired].as<float>());
        if (vars[ptName].defaulted()) {
            options->graphics.*ptMember = value;
        }
        if (vars[pbrName].defaulted()) {
            options->graphics.*pbrMember = value;
        }
    };
    retiredDial("lightmapintensity", "ptlightmapintensity", "pbrlightmapintensity",
                &GraphicsOptions::ptLightmapIntensity, &GraphicsOptions::pbrLightmapIntensity);
    // A retired absolute roughness pinned both ends of the reflective mask.
    for (int i = 0; i < 9; ++i) {
        auto &refl = options->graphics.categoryOverrides[i].reflective;
        const auto key = "cat" + std::to_string(i) + "refl";
        const float retiredRoughness = vars[key + "roughness"].as<float>();
        if (retiredRoughness >= 0.0f && vars[key + "roughness0"].defaulted() &&
            vars[key + "roughness1"].defaulted()) {
            refl.roughness[0] = refl.roughness[1] = std::min(retiredRoughness, 1.0f);
        }
    }
    // The retired global dials of the traced grade, carried onto the material
    // dials they became, so a reone.cfg written before them keeps its look.
    // Command-line values for the new keys win over a migrated one.
    {
        auto &g = options->graphics;
        const float sky = vars["ptskyintensity"].as<float>();
        if (sky >= 0.0f && vars["skyboxintensity"].defaulted()) {
            g.skyboxIntensity = sky;
        }
        const float emissive = vars["ptemissiveintensity"].as<float>();
        const float gamma = vars["emissivegamma"].as<float>();
        const float backdrop = vars["ptbackdropintensity"].as<float>();
        const auto migrate = [&](graphics::GraphicsOptions::EmissionOverride &e, const std::string &key, float intensity) {
            if (intensity >= 0.0f && vars[key + "on"].defaulted() && vars[key + "intensity"].defaulted()) {
                e.enabled = true;
                e.intensity = intensity;
            }
            if (gamma >= 0.0f && vars[key + "gamma"].defaulted() && e.enabled) {
                e.gamma = std::clamp(gamma, 0.1f, 4.0f);
            }
        };
        for (int i = 0; i < 9; ++i) {
            migrate(g.categoryOverrides[i].emission, "cat" + std::to_string(i) + "emission", emissive);
        }
        migrate(g.skyRoomEmission, "skyroomemission", backdrop);
        // The retired single-class keys belong to the reflective class: rough
        // surfaces had no lobe, so nothing they scaled ever reached one.
        for (int i = 0; i < 9; ++i) {
            const auto key = "cat" + std::to_string(i);
            auto &category = g.categoryOverrides[i];
            auto &refl = category.reflective;
            const auto fresh = [&](const char *field) { return vars[key + "refl" + field].defaulted(); };
            for (int c = 0; c < 3; ++c) {
                const float v = vars[key + "color" + std::to_string(c)].as<float>();
                if (v >= 0.0f && fresh(("color" + std::to_string(c)).c_str())) refl.color[c] = v;
            }
            const float weight = vars[key + "colorweight"].as<float>();
            if (weight >= 0.0f && fresh("colorweight")) refl.colorWeight = std::clamp(weight, 0.0f, 1.0f);
            const float roughness = vars[key + "roughness"].as<float>();
            if (roughness >= 0.0f && fresh("roughness0") && fresh("roughness1")) {
                refl.roughness[0] = refl.roughness[1] = std::min(roughness, 1.0f);
            }
            const float emissionScale = vars[key + "emission"].as<float>();
            if (emissionScale >= 0.0f && vars[key + "emissionon"].defaulted() && vars[key + "emissionintensity"].defaulted()) {
                category.emission.enabled = true;
                category.emission.intensity = (emissive >= 0.0f ? emissive : 1.0f) * emissionScale;
            }
        }
    }
    // Resolved here, where the render mode is also known, so that nothing
    // deeper has to ask again: below this point the option says what the slot
    // runs, full stop. A traced frame is noisy and carries the motion a
    // temporal resolve wants, so it takes FSR; raster keeps the cheap spatial
    // filter it has always had. An explicit flag always wins.
    if (vars["antialiasing"].defaulted()) {
        options->graphics.antialiasing = options->graphics.mode == RenderMode::PathTracing
                                             ? AntiAliasing::Fsr
                                             : AntiAliasing::Fxaa;
    }
    // The dial the post-process pass reads is `grade`: that pass is no longer
    // optional - it is the frame's only encode - so what is switchable is the
    // exposure and the tone curve it applies. `post` is the name that dial had
    // when it did switch the pass itself, kept working here rather than broken
    // for every script and commands file already written against it. An
    // explicit --grade wins; otherwise --post is read, and it defaults to the
    // same value, so a run that passes neither is unaffected.
    if (vars["grade"].defaulted()) {
        options->graphics.grade = vars["post"].as<bool>();
    }
    // Same shape as --grade / --post above: the dial dropped its pt prefix when
    // the views stopped being tracer-only, and the old spelling keeps working
    // rather than breaking every script already written against it. An explicit
    // --debugview wins; otherwise --ptdebugview is read, and both default to
    // the same value, so a run that passes neither is unaffected.
    if (vars["debugview"].defaulted()) {
        options->graphics.debugView =
            std::clamp(vars["ptdebugview"].as<int>(), 0, graphics::kMaxDebugView);
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
    options->recordInput = vars["record-input"].as<std::string>();

    return options;
}

} // namespace reone
