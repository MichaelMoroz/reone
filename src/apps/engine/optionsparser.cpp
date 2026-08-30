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
#include <cmath>
#include <stdexcept>
#include <string>

#include "reone/graphics/optionsregistry.h"
#include "reone/system/logutil.h"
#include "reone/system/types.h"

using namespace boost::program_options;

using namespace reone::game;
using namespace reone::graphics;

namespace reone {

static constexpr char kConfigFilename[] = "reone.cfg";

static float positiveFiniteScale(const variables_map &vars, const char *name) {
    float value = vars[name].as<float>();
    if (!std::isfinite(value) || value <= 0.0f) {
        throw std::invalid_argument(std::string("--") + name + " must be finite and greater than zero");
    }
    return value;
}

/** The written form of the anti-aliasing slot, as reone.cfg stores it. */
static const char *antiAliasingName(AntiAliasing value) {
    switch (value) {
    case AntiAliasing::Fxaa:
        return "fxaa";
    case AntiAliasing::Fsr:
        return "fsr";
    case AntiAliasing::DlssRr:
        return "dlssrr";
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
    if (value == "dlssrr") {
        return AntiAliasing::DlssRr;
    }
    throw std::invalid_argument("Unknown anti-aliasing mode '" + value +
                                "'; expected off, fxaa, fsr or dlssrr");
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
        ("record-input", value<std::string>()->default_value(""), "record this session's input for later replay")             //
        ("dumptargets", value<std::string>()->default_value(""), "write the scene render targets to this directory as .npy")   //
        ("dumpobjects", value<std::string>()->default_value(""), "append the module's traced-emissive candidates to this file") //
        ("randomseed", value<int>()->default_value(-1), "seed the random generator, or -1 to seed from the clock")             //
        ("vkvalidation", value<bool>()->default_value(false), "enable Vulkan validation layers")                              //
        ("vkdebuglabels", value<bool>()->default_value(false), "label Vulkan passes for GPU profilers; not with capture")      //
        ("renderdoc", value<bool>()->default_value(false), "trigger a RenderDoc frame capture with the screenshot")            //
        ("dev", value<bool>()->default_value(options->game.developer), "enable developer mode")                                 //
        ("width", value<int>()->default_value(options->graphics.width), "render width")                                         //
        ("height", value<int>()->default_value(options->graphics.height), "render height")                                      //
        ("winscale", value<int>()->default_value(options->graphics.winScale), "window scale")                                   //
        ("fullscreen", value<bool>()->default_value(options->graphics.fullscreen), "enable fullscreen")                         //
        ("headless", value<bool>()->default_value(false), "never show the window; for scripted batch runs")                     //
        ("vsync", value<bool>()->default_value(options->graphics.vsync), "enable v-sync")                                       //
        ("guiscale", value<float>()->default_value(options->graphics.guiScale), "GUI layout scale")                            //
        ("guitextscale", value<float>()->default_value(options->graphics.guiTextScale), "GUI text scale")                      //
        ("guidialogtextscale", value<float>()->default_value(options->graphics.guiDialogTextScale), "dialog text scale")      //
        ("guiborderscale", value<float>()->default_value(options->graphics.guiBorderScale), "GUI border scale")                //
        ("guilistscale", value<float>()->default_value(options->graphics.guiListScale), "GUI list row scale")                  //
        ("grass", value<bool>()->default_value(options->graphics.grass), "enable grass")                                        //
        ("grassdensity", value<float>()->default_value(options->graphics.grassDensity), "grass density multiplier")           //
        ("shadows", value<bool>()->default_value(options->graphics.shadows), "enable shadows")                                  //
        ("fog", value<bool>()->default_value(options->graphics.fog), "enable distance fog")                                     //
        ("fogheight", value<float>()->default_value(options->graphics.fogHeight), "fog gradient height in world units") //
        ("particles", value<bool>()->default_value(options->graphics.particles), "enable emitter particles")                    //
        ("lensflares", value<bool>()->default_value(options->graphics.lensFlares), "draw light halo billboards")                //
        ("bloom", value<bool>()->default_value(options->graphics.bloom), "bloom emissive highlights")                           //
        ("bloomthreshold", value<float>()->default_value(options->graphics.bloomThreshold), "level a texel must pass to bloom") //
        ("bloomintensity", value<float>()->default_value(options->graphics.bloomIntensity), "scale on what bloom adds back")    //
        ("thintransmission", value<float>()->default_value(options->graphics.thinTransmission),
         "light a thin surface (leaf, cloth, grass) passes to its far side")                                                  //
        ("ptrefraction", value<float>()->default_value(options->graphics.ptRefraction),
         "how far a traced ray bends passing through a transparent surface; 0 is straight")                                   //
        ("mode", value<std::string>()->default_value(renderModeName(options->graphics.mode)), "render mode: retro, pbr or path-tracing ('raster' is accepted as a spelling of retro)") //
        ("admissionshadow", value<bool>()->default_value(false), "compare incremental and full scene admission every frame") //
        ("admissionforcefull", value<bool>()->default_value(false), "force full scene collection and classification")        //
        ("ptspp", value<int>()->default_value(options->graphics.pathTracingSamples), "path tracing samples per pixel")          //
        ("skyintensity", value<float>(), "deprecated: use ptskyintensity / pbrskyintensity") //
        ("emissiveintensity", value<float>(), "deprecated: use ptemissiveintensity / pbremissiveintensity") //
        ("lightmapintensity", value<float>(), "deprecated: use ptlightmapintensity / pbrlightmapintensity") //
        ("skyboxintensity", value<float>()->default_value(options->graphics.skyboxIntensity), "baked sky cube intensity")      //
        ("skyboxgamma", value<float>()->default_value(options->graphics.skyboxGamma), "baked sky cube decode exponent")     //
        ("ptlightmapintensity", value<float>()->default_value(options->graphics.ptLightmapIntensity), "path tracing: baked-lightmap intensity") //
        ("pbrlightmapintensity", value<float>()->default_value(options->graphics.pbrLightmapIntensity), "PBR: baked-lightmap intensity") //
        ("pbrdirectintensity", value<float>()->default_value(options->graphics.pbrDirectIntensity), "PBR: direct light intensity") //
        ("pbrsunintensity", value<float>()->default_value(options->graphics.pbrSunIntensity), "PBR: sun intensity") //
        ("ptdirectintensity", value<float>()->default_value(options->graphics.ptDirectIntensity), "path tracing direct-light intensity") //
        ("ptsunintensity", value<float>()->default_value(options->graphics.ptSunIntensity), "path tracing sun intensity")       //
        ("ptbounces", value<int>()->default_value(options->graphics.ptBounces), "path tracing bounces")                       //
        ("ptnee", value<bool>()->default_value(options->graphics.ptNee), "next-event estimation")                              //
        ("ptneesamples", value<int>()->default_value(options->graphics.ptNeeSamples), "light samples per shading vertex")      //
        ("ptgrassscatter", value<bool>()->default_value(options->graphics.ptGrassScatter), "scatter rays see grass")           //
        ("ptgrassshadows", value<bool>()->default_value(options->graphics.ptGrassShadows), "shadow rays see grass")           //
        ("ptrayoffset", value<float>()->default_value(options->graphics.ptRayOffset), "path tracing ray origin offset")        //
        ("pttracestats", value<bool>()->default_value(options->graphics.ptTraceStats), "enable path tracing statistics")       //
        ("tonemap", value<int>()->default_value(options->graphics.tonemap), "display transform: 0 off, 1 Gran Turismo curve")               //
        ("exposure", value<float>()->default_value(options->graphics.exposure), "scene-referred exposure ahead of the tonemap") //
        ("ptpointemitterratio", value<float>()->default_value(options->graphics.ptPointEmitterRatio), "path tracing point-light emitter radius, as a fraction of influence radius") //
        ("ptbounceroughness", value<float>()->default_value(options->graphics.ptBounceRoughness), "roughness floor after the first scatter (path regularisation)") //
        ("ptroughnessfloor", value<float>()->default_value(options->graphics.ptRoughnessFloor), "lowest roughness any surface may take") //
        ("ptindirectclamp", value<float>()->default_value(options->graphics.ptIndirectClamp), "ceiling on one indirect sample, 0 to disable") //
        ("ptsunangularsize", value<float>()->default_value(options->graphics.ptSunAngularSize), "path tracing sun angular size") //
        ("albedogamma", value<float>()->default_value(options->graphics.albedoGamma), "authored albedo decode exponent, PBR and path tracing alike (2.2 is sRGB-correct, 1.0 matches the reference engines)") //
        ("maxlights", value<int>()->default_value(options->graphics.maxLights), "lights a frame may carry")                    //
        ("maxdirectionalshadows", value<int>()->default_value(options->graphics.maxDirectionalShadows),
         "shadow-casting directional lights (PBR and path tracing)")                                                          //
        ("maxpointshadows", value<int>()->default_value(options->graphics.maxPointShadows),
         "shadow-casting point lights (PBR and path tracing)")                                                                //
        ("pointshadowres", value<int>()->default_value(options->graphics.pointShadowResolution),
         "point shadow cube face resolution")                                                                                 //
        ("lightmaps", value<bool>()->default_value(options->graphics.lightmaps), "apply lightmaps (diagnostic toggle)")        //
        ("ssao", value<bool>()->default_value(options->graphics.ssao), "enable screen-space ambient occlusion")                 //
        ("ssr", value<bool>()->default_value(options->graphics.ssr), "enable screen-space reflections")                         //
        ("debugoverlay", value<bool>()->default_value(options->graphics.debugOverlay), "bounding boxes and names of objects and lights, over the image") //
        ("antialiasing", value<std::string>()->default_value(antiAliasingName(options->graphics.antialiasing)),
         "anti-aliasing in the common slot: off, fxaa or fsr; defaults per render mode")                                       //
        ("grade", value<bool>()->default_value(options->graphics.grade),
         "apply exposure and the tone curve; the display transform itself always runs")                                        //
        ("post", value<bool>()->default_value(options->graphics.grade),
         "deprecated alias for --grade")                                                                                       //
        ("paritydirect", value<bool>()->default_value(options->graphics.parityDirect), "both renderers output only shared unoccluded direct diffuse") //
        ("ptdenoise", value<bool>()->default_value(options->graphics.ptDenoise), "enable the path tracing denoiser")           //
        ("ptshadowfilter", value<std::string>()->default_value("off"),
         "what settles the direct channel: off, penumbra or denoiser")                                                        //
        ("ptnrddirectaccumtime", value<float>()->default_value(options->graphics.ptNrdDirectAccumulationTime),
         "direct-light denoiser history, seconds")                                                                            //
        ("ptnrddirectatrous", value<int>()->default_value(options->graphics.ptNrdDirectAtrousIterations),
         "direct-light denoiser A-trous iterations")                                                                          //
        ("ptnrddirectphiluminance", value<float>()->default_value(options->graphics.ptNrdDirectPhiLuminance),
         "direct-light denoiser luminance edge stopping")                                                                     //
        ("grassradius", value<float>()->default_value(options->graphics.grassRadius), "grass draw radius")             //
        ("grasscardshape", value<std::string>()->default_value(grassCardShapeName(options->graphics.grassCardShape)), "fitted grass card outline") //
        ("grasscardsides", value<int>()->default_value(options->graphics.grassCardSides), "sides of a fitted grass k-gon") //
        ("grasscardgrid", value<int>()->default_value(options->graphics.grassCardGrid), "cells across a fitted grass grid") //
        ("grasscardaspect", value<float>()->default_value(options->graphics.grassCardAspect), "grass card width over length") //
        ("grasswindstrength", value<float>()->default_value(options->graphics.grassWindStrength), "wind bend, radians") //
        ("grasswinddirection", value<float>()->default_value(options->graphics.grassWindDirection), "wind direction, radians") //
        ("grasswindspeed", value<float>()->default_value(options->graphics.grassWindSpeed), "wind rustle speed")          //
        ("grasswindwavelength", value<float>()->default_value(options->graphics.grassWindWavelength), "wind crest spacing") //
        ("grasswindgust", value<float>()->default_value(options->graphics.grassWindGust), "gust share of the strength")   //
        ("grassorientation", value<float>()->default_value(options->graphics.grassOrientation), "blade facing, radians") //
        ("grassorientationvariance", value<float>()->default_value(options->graphics.grassOrientationVariance),
         "spread around that facing, radians; 0 aligns them all")                                                             //
        ("grasscurvature", value<float>()->default_value(options->graphics.grassCurvature), "blade bend, radians")       //
        ("grasscurvaturevariance", value<float>()->default_value(options->graphics.grassCurvatureVariance), "bend variance") //
        ("grasssparsity", value<float>()->default_value(options->graphics.grassSparsity), "fraction of slots left empty") //
        ("grassdisplacement", value<float>()->default_value(options->graphics.grassDisplacement), "slot displacement, cells") //
        ("grasslength", value<float>()->default_value(options->graphics.grassLength), "blade length x authored quad size") //
        ("grasslengthvariance", value<float>()->default_value(options->graphics.grassLengthVariance), "length variance")  //
        ("grasswidth", value<float>()->default_value(options->graphics.grassWidth), "blade width, fraction of length")   //
        ("grassyoffset", value<float>()->default_value(options->graphics.grassYOffset), "root offset, fraction of length") //
        ("grasstrianglebudget", value<int>()->default_value(options->graphics.grassTriangleBudget), "grass triangle ceiling") //
        ("grassroughness", value<float>()->default_value(options->graphics.grassRoughness), "blade roughness")            //
        ("grassbladespercluster", value<int>()->default_value(options->graphics.grassBladesPerCluster), "blades grown per authored cluster") //
        ("grasscolor", value<std::string>()->default_value(""), "blade albedo as \"r g b\"")                                 //
        ("ptdirectchannel", value<bool>()->default_value(options->graphics.ptDirectChannel),
         "apply primary-vertex direct light at the resolve instead of through the denoiser")                                  //
        ("debugview", value<int>()->default_value(options->graphics.debugView),
         "debug channel view in any render mode, 0 off")                                                                  //
        ("ptdebugview", value<int>()->default_value(options->graphics.debugView),
         "deprecated alias for --debugview")                                                                              //
        ("ptnrdaccumtime", value<float>()->default_value(options->graphics.ptNrdAccumulationTime),
         "denoiser history, seconds")                                                                                         //
        ("ptnrdfastaccumtime", value<float>()->default_value(options->graphics.ptNrdFastAccumulationTime),
         "denoiser responsive history, seconds")                                                                              //
        ("ptnrdatrous", value<int>()->default_value(options->graphics.ptNrdAtrousIterations),
         "RELAX a-trous iterations")                                                                                          //
        ("ptnrddiffusephi", value<float>()->default_value(options->graphics.ptNrdDiffusePhiLuminance),
         "RELAX diffuse luminance edge stopper")                                                                              //
        ("ptnrdspecularphi", value<float>()->default_value(options->graphics.ptNrdSpecularPhiLuminance),
         "RELAX specular luminance edge stopper")                                                                             //
        ("ptnrddepththreshold", value<float>()->default_value(options->graphics.ptNrdDepthThreshold),
         "RELAX depth threshold for spatial passes")                                                                          //
        ("ptnrdspecularlobeslack", value<float>()->default_value(options->graphics.ptNrdSpecularLobeAngleSlack),
         "RELAX specular lobe angle slack, degrees")                                                                          //
        ("ptnrdhistoryfix", value<int>()->default_value(options->graphics.ptNrdHistoryFixFrames),
         "denoiser history fix frames")                                                                                         //
        ("ptnrddiffuseprepassblurradius", value<float>()->default_value(options->graphics.ptNrdDiffusePrepassBlurRadius),
         "denoiser diffuse prepass blur radius")                                                                                //
        ("ptnrdspecularprepassblurradius", value<float>()->default_value(options->graphics.ptNrdSpecularPrepassBlurRadius),
         "denoiser specular prepass blur radius")                                                                               //
        ("ptnrdlobeanglefraction", value<float>()->default_value(options->graphics.ptNrdLobeAngleFraction),
         "denoiser lobe angle fraction")                                                                                        //
        ("ptnrdroughnessfraction", value<float>()->default_value(options->graphics.ptNrdRoughnessFraction),
         "denoiser roughness fraction")                                                                                         //
        ("ptnrddisocclusionthreshold", value<float>()->default_value(options->graphics.ptNrdDisocclusionThreshold),
         "denoiser disocclusion threshold")                                                                                     //
        ("ptnrdantifirefly", value<bool>()->default_value(options->graphics.ptNrdAntiFirefly),
         "enable denoiser anti-firefly")                                                                                        //
        ("renderscale", value<float>()->default_value(options->graphics.renderScale),
         "trace/raster resolution as a fraction of display; FSR upscales (1 = NativeAA)")             //
        ("dlssmode", value<std::string>()->default_value("dlaa"),
         "DLSS quality mode, which is how DLSS names a render scale: "
         "dlaa, quality, balanced, performance or ultraperformance")                                //
        ("sharpness", value<float>()->default_value(options->graphics.sharpness),
         "sharpening, 0 disables it: RCAS under FSR, DLSS-RR's own under DLSS, "
         "an unsharp mask under neither")                                                                          //
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

    const auto addSurfaceClass = [&](const std::string &key, const graphics::GraphicsOptions::SurfaceClassOverride &c) {
        descCommon.add_options()                                                                                        //
            ((key + "color0").c_str(), value<float>()->default_value(c.color[0]), "material override colour red")       //
            ((key + "color1").c_str(), value<float>()->default_value(c.color[1]), "material override colour green")     //
            ((key + "color2").c_str(), value<float>()->default_value(c.color[2]), "material override colour blue")      //
            ((key + "colorweight").c_str(), value<float>()->default_value(c.colorWeight), "material override colour weight") //
            ((key + "roughness").c_str(), value<float>()->default_value(c.roughness), "material override roughness")   //
            ((key + "metallic").c_str(), value<float>()->default_value(c.metallic), "material override metalness")     //
            ((key + "f0").c_str(), value<float>()->default_value(c.f0), "material override reflectance at normal incidence") //
            ((key + "roughnessscale").c_str(), value<float>()->default_value(c.roughnessScale), "material override roughness scale") //
            ((key + "metallicscale").c_str(), value<float>()->default_value(c.metallicScale), "material override metalness scale");
    };
    const auto addEmission = [&](const std::string &key, const graphics::GraphicsOptions::EmissionOverride &e) {
        descCommon.add_options()                                                                                  //
            ((key + "on").c_str(), value<bool>()->default_value(e.enabled), "emission grade on")                  //
            ((key + "intensity").c_str(), value<float>()->default_value(e.intensity), "emission intensity")       //
            ((key + "gamma").c_str(), value<float>()->default_value(e.gamma), "emission decode exponent");
    };
    for (int i = 0; i < 9; ++i) {
        const auto &override = options->graphics.categoryOverrides[i];
        const auto key = "cat" + std::to_string(i);
        addSurfaceClass(key + "rough", override.rough);
        addSurfaceClass(key + "refl", override.reflective);
        addEmission(key + "emission", override.emission);
    }
    addEmission("skyroomemission", options->graphics.skyRoomEmission);

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
    options->dumpTargetsPath = vars["dumptargets"].as<std::string>();
    // The per-frame upload hash exists for the dump log line; do not pay for
    // it on frames nobody will ever compare.
    options->graphics.hashUploads = !options->dumpTargetsPath.empty();
    options->dumpObjectsPath = vars["dumpobjects"].as<std::string>();
    options->randomSeed = vars["randomseed"].as<int>();
    options->vulkanValidation = vars["vkvalidation"].as<bool>();
    options->vulkanDebugLabels = vars["vkdebuglabels"].as<bool>();
    options->renderdoc = vars["renderdoc"].as<bool>();
    options->graphics.headless = vars["headless"].as<bool>();
    options->audio.muted = options->graphics.headless;
    options->graphics.width = vars["width"].as<int>();
    options->graphics.height = vars["height"].as<int>();
    options->graphics.winScale = vars["winscale"].as<int>();
    options->graphics.fullscreen = vars["fullscreen"].as<bool>();
    options->graphics.vsync = vars["vsync"].as<bool>();
    options->graphics.guiScale = positiveFiniteScale(vars, "guiscale");
    options->graphics.guiTextScale = positiveFiniteScale(vars, "guitextscale");
    options->graphics.guiDialogTextScale = positiveFiniteScale(vars, "guidialogtextscale");
    options->graphics.guiBorderScale = positiveFiniteScale(vars, "guiborderscale");
    options->graphics.guiListScale = positiveFiniteScale(vars, "guilistscale");
    options->graphics.grass = vars["grass"].as<bool>();
    options->graphics.grassDensity = vars["grassdensity"].as<float>();
    options->graphics.shadows = vars["shadows"].as<bool>();
    options->graphics.fog = vars["fog"].as<bool>();
    options->graphics.fogHeight = std::max(0.05f, vars["fogheight"].as<float>());
    options->graphics.particles = vars["particles"].as<bool>();
    options->graphics.lensFlares = vars["lensflares"].as<bool>();
    options->graphics.bloom = vars["bloom"].as<bool>();
    options->graphics.bloomThreshold = vars["bloomthreshold"].as<float>();
    options->graphics.bloomIntensity = std::max(0.0f, vars["bloomintensity"].as<float>());
    options->graphics.thinTransmission = std::clamp(vars["thintransmission"].as<float>(), 0.0f, 1.0f);
    options->graphics.ptRefraction = std::clamp(vars["ptrefraction"].as<float>(), 0.0f, 1.0f);
    options->graphics.mode = parseRenderMode(vars["mode"].as<std::string>());
    options->graphics.admissionShadow = vars["admissionshadow"].as<bool>();
    options->graphics.admissionForceFull = vars["admissionforcefull"].as<bool>();
    options->graphics.pathTracingSamples = std::max(1, vars["ptspp"].as<int>());
    // A dial per mode, read straight through. There is no per-mode default
    // resolution here any more: the two grades are authored against different
    // transport, so a shared number made every adjustment a question of which
    // mode happened to be running.
    options->graphics.skyboxIntensity = std::max(0.0f, vars["skyboxintensity"].as<float>());
    options->graphics.skyboxGamma = std::clamp(vars["skyboxgamma"].as<float>(), 0.1f, 4.0f);
    options->graphics.ptLightmapIntensity =
        std::max(0.0f, vars["ptlightmapintensity"].as<float>());
    options->graphics.pbrLightmapIntensity =
        std::max(0.0f, vars["pbrlightmapintensity"].as<float>());
    options->graphics.ptDirectIntensity = vars["ptdirectintensity"].as<float>();
    options->graphics.pbrDirectIntensity = vars["pbrdirectintensity"].as<float>();
    options->graphics.ptSunIntensity = vars["ptsunintensity"].as<float>();
    options->graphics.pbrSunIntensity = vars["pbrsunintensity"].as<float>();
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
    options->graphics.ptBounceRoughness = std::clamp(vars["ptbounceroughness"].as<float>(), 0.0f, 1.0f);
    options->graphics.ptRoughnessFloor = std::clamp(vars["ptroughnessfloor"].as<float>(), 0.0f, 1.0f);
    options->graphics.ptIndirectClamp = std::max(0.0f, vars["ptindirectclamp"].as<float>());
    options->graphics.ptBounces = std::clamp(vars["ptbounces"].as<int>(),
                                             graphics::kMinPtBounces, graphics::kMaxPtBounces);
    options->graphics.ptNee = vars["ptnee"].as<bool>();
    options->graphics.ptNeeSamples = std::clamp(vars["ptneesamples"].as<int>(),
                                                graphics::kMinPtNeeSamples,
                                                graphics::kMaxPtNeeSamples);
    options->graphics.ptGrassScatter = vars["ptgrassscatter"].as<bool>();
    options->graphics.ptGrassShadows = vars["ptgrassshadows"].as<bool>();
    const auto readSurfaceClass = [&](const std::string &key, graphics::GraphicsOptions::SurfaceClassOverride &c) {
        for (int k = 0; k < 3; ++k) c.color[k] = vars[key + "color" + std::to_string(k)].as<float>();
        c.colorWeight = std::clamp(vars[key + "colorweight"].as<float>(), 0.0f, 1.0f);
        c.roughness = std::clamp(vars[key + "roughness"].as<float>(), -1.0f, 1.0f);
        c.metallic = std::clamp(vars[key + "metallic"].as<float>(), -1.0f, 1.0f);
        c.f0 = std::clamp(vars[key + "f0"].as<float>(), 0.0f, 1.0f);
        c.roughnessScale = std::max(0.0f, vars[key + "roughnessscale"].as<float>());
        c.metallicScale = std::max(0.0f, vars[key + "metallicscale"].as<float>());
    };
    const auto readEmission = [&](const std::string &key, graphics::GraphicsOptions::EmissionOverride &e) {
        e.enabled = vars[key + "on"].as<bool>();
        e.intensity = std::max(0.0f, vars[key + "intensity"].as<float>());
        e.gamma = std::clamp(vars[key + "gamma"].as<float>(), 0.1f, 4.0f);
    };
    for (int i = 0; i < 9; ++i) {
        auto &override = options->graphics.categoryOverrides[i];
        const auto key = "cat" + std::to_string(i);
        readSurfaceClass(key + "rough", override.rough);
        readSurfaceClass(key + "refl", override.reflective);
        readEmission(key + "emission", override.emission);
    }
    readEmission("skyroomemission", options->graphics.skyRoomEmission);
    options->graphics.ptRayOffset = std::max(0.0001f, vars["ptrayoffset"].as<float>());
    options->graphics.ptTraceStats = vars["pttracestats"].as<bool>();
    options->graphics.tonemap = std::clamp(vars["tonemap"].as<int>(), 0, 1);
    options->graphics.exposure = std::max(0.05f, vars["exposure"].as<float>());
    options->graphics.ptPointEmitterRatio = std::clamp(vars["ptpointemitterratio"].as<float>(), 0.01f, 0.5f);
    options->graphics.ptSunAngularSize = std::max(0.05f, vars["ptsunangularsize"].as<float>());
    options->graphics.albedoGamma = std::clamp(vars["albedogamma"].as<float>(), 0.1f, 4.0f);
    options->graphics.maxLights = std::clamp(vars["maxlights"].as<int>(), 1, graphics::kMaxLights);
    options->graphics.maxDirectionalShadows =
        std::clamp(vars["maxdirectionalshadows"].as<int>(), 0, 4);
    options->graphics.maxPointShadows = std::clamp(vars["maxpointshadows"].as<int>(), 0, kMaxPointShadows);
    options->graphics.pointShadowResolution =
        std::clamp(vars["pointshadowres"].as<int>(), 128, 2048);
    options->graphics.lightmaps = vars["lightmaps"].as<bool>();
    options->graphics.ssao = vars["ssao"].as<bool>();
    options->graphics.ssr = vars["ssr"].as<bool>();
    options->graphics.debugOverlay = vars["debugoverlay"].as<bool>();
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
    options->graphics.parityDirect = vars["paritydirect"].as<bool>();
    options->graphics.ptDenoise = vars["ptdenoise"].as<bool>();
    // Same shape as --grade / --post above: the dial dropped its pt prefix when
    // the views stopped being tracer-only, and the old spelling keeps working
    // rather than breaking every script already written against it. An explicit
    // --debugview wins; otherwise --ptdebugview is read, and both default to
    // the same value, so a run that passes neither is unaffected.
    options->graphics.debugView =
        std::clamp(vars["debugview"].defaulted() ? vars["ptdebugview"].as<int>()
                                                 : vars["debugview"].as<int>(),
                   0, graphics::kMaxDebugView);
    options->graphics.ptDirectChannel = vars["ptdirectchannel"].as<bool>();
    {
        // Rejected rather than silently taken as "off", for the same reason the
        // anti-aliasing slot is: a typo that quietly disables the thing you
        // were measuring is indistinguishable from the thing not working.
        const auto value = vars["ptshadowfilter"].as<std::string>();
        if (value == "off" || value == "none" || value == "0") {
            options->graphics.ptShadowFilter = graphics::ShadowFilter::Off;
        } else if (value == "denoiser" || value == "nrd" || value == "1") {
            options->graphics.ptShadowFilter = graphics::ShadowFilter::Denoiser;
        } else {
            throw std::invalid_argument("Unknown shadow filter '" + value +
                                        "'; expected off or denoiser");
        }
    }
    options->graphics.ptNrdDirectAccumulationTime =
        std::clamp(vars["ptnrddirectaccumtime"].as<float>(), 0.0f, 2.0f);
    options->graphics.ptNrdDirectAtrousIterations =
        std::clamp(vars["ptnrddirectatrous"].as<int>(), 2, 8);
    options->graphics.ptNrdDirectPhiLuminance =
        std::clamp(vars["ptnrddirectphiluminance"].as<float>(), 0.0f, 16.0f);
    options->graphics.grassRadius = std::max(0.0f, vars["grassradius"].as<float>());
    options->graphics.grassCardShape = parseGrassCardShape(vars["grasscardshape"].as<std::string>());
    options->graphics.grassCardSides = std::clamp(vars["grasscardsides"].as<int>(), 3, 16);
    options->graphics.grassCardGrid = std::clamp(vars["grasscardgrid"].as<int>(), 2, 32);
    options->graphics.grassCardAspect = std::clamp(vars["grasscardaspect"].as<float>(), 0.1f, 4.0f);
    options->graphics.grassWindStrength = std::clamp(vars["grasswindstrength"].as<float>(), 0.0f, 2.0f);
    options->graphics.grassWindDirection =
        std::clamp(vars["grasswinddirection"].as<float>(), -6.2832f, 6.2832f);
    options->graphics.grassWindSpeed = std::clamp(vars["grasswindspeed"].as<float>(), 0.0f, 10.0f);
    options->graphics.grassWindWavelength =
        std::clamp(vars["grasswindwavelength"].as<float>(), 0.1f, 64.0f);
    options->graphics.grassWindGust = std::clamp(vars["grasswindgust"].as<float>(), 0.0f, 1.0f);
    options->graphics.grassOrientation =
        std::clamp(vars["grassorientation"].as<float>(), -6.2832f, 6.2832f);
    options->graphics.grassOrientationVariance =
        std::clamp(vars["grassorientationvariance"].as<float>(), 0.0f, 6.2832f);
    options->graphics.grassCurvature = std::clamp(vars["grasscurvature"].as<float>(), -2.0f, 2.0f);
    options->graphics.grassCurvatureVariance = std::clamp(vars["grasscurvaturevariance"].as<float>(), 0.0f, 2.0f);
    options->graphics.grassSparsity = std::clamp(vars["grasssparsity"].as<float>(), 0.0f, 0.99f);
    options->graphics.grassDisplacement = std::clamp(vars["grassdisplacement"].as<float>(), 0.0f, 3.0f);
    options->graphics.grassLength = std::clamp(vars["grasslength"].as<float>(), 0.0f, 8.0f);
    options->graphics.grassLengthVariance = std::clamp(vars["grasslengthvariance"].as<float>(), 0.0f, 1.0f);
    options->graphics.grassWidth = std::clamp(vars["grasswidth"].as<float>(), 0.0f, 1.0f);
    options->graphics.grassYOffset = std::clamp(vars["grassyoffset"].as<float>(), -1.0f, 1.0f);
    options->graphics.grassTriangleBudget =
        std::clamp(vars["grasstrianglebudget"].as<int>(), 0, graphics::kMaxGrassTriangleBudget);
    options->graphics.grassRoughness = std::clamp(vars["grassroughness"].as<float>(), 0.0f, 1.0f);
    options->graphics.grassBladesPerCluster = std::clamp(vars["grassbladespercluster"].as<int>(), 1, 32);
    if (const auto color = vars["grasscolor"].as<std::string>(); !color.empty()) {
        std::istringstream stream(color);
        glm::vec3 parsed {0.0f};
        if (stream >> parsed.r >> parsed.g >> parsed.b) {
            options->graphics.grassColor = glm::max(parsed, glm::vec3(0.0f));
        }
    }
    options->graphics.ptNrdAccumulationTime = std::clamp(vars["ptnrdaccumtime"].as<float>(), 0.0f, 2.0f);
    options->graphics.ptNrdFastAccumulationTime = std::clamp(vars["ptnrdfastaccumtime"].as<float>(), 0.0f, 2.0f);
    options->graphics.ptNrdAtrousIterations = std::clamp(vars["ptnrdatrous"].as<int>(), 2, 8);
    options->graphics.ptNrdDiffusePhiLuminance = std::max(0.0f, vars["ptnrddiffusephi"].as<float>());
    options->graphics.ptNrdSpecularPhiLuminance = std::max(0.0f, vars["ptnrdspecularphi"].as<float>());
    options->graphics.ptNrdDepthThreshold = std::max(0.0f, vars["ptnrddepththreshold"].as<float>());
    options->graphics.ptNrdSpecularLobeAngleSlack = std::max(0.0f, vars["ptnrdspecularlobeslack"].as<float>());
    options->graphics.ptNrdHistoryFixFrames = std::max(0, vars["ptnrdhistoryfix"].as<int>());
    options->graphics.ptNrdDiffusePrepassBlurRadius = std::max(0.0f, vars["ptnrddiffuseprepassblurradius"].as<float>());
    options->graphics.ptNrdSpecularPrepassBlurRadius = std::max(0.0f, vars["ptnrdspecularprepassblurradius"].as<float>());
    options->graphics.ptNrdLobeAngleFraction = std::clamp(vars["ptnrdlobeanglefraction"].as<float>(), 0.01f, 1.0f);
    options->graphics.ptNrdRoughnessFraction = std::clamp(vars["ptnrdroughnessfraction"].as<float>(), 0.01f, 1.0f);
    options->graphics.ptNrdDisocclusionThreshold = std::max(0.0f, vars["ptnrddisocclusionthreshold"].as<float>());
    options->graphics.ptNrdAntiFirefly = vars["ptnrdantifirefly"].as<bool>();
    options->graphics.renderScale = std::clamp(vars["renderscale"].as<float>(), 0.25f, 1.0f);
    options->graphics.sharpness = std::clamp(vars["sharpness"].as<float>(), 0.0f, 1.0f);
    {
        // Rejected rather than defaulted, on the same rule as the slot itself:
        // a typo that quietly renders at a different resolution than asked for
        // is indistinguishable from DLSS behaving oddly.
        const auto value = vars["dlssmode"].as<std::string>();
        if (value == "dlaa" || value == "native") {
            options->graphics.dlssMode = graphics::DlssMode::Dlaa;
        } else if (value == "quality") {
            options->graphics.dlssMode = graphics::DlssMode::Quality;
        } else if (value == "balanced") {
            options->graphics.dlssMode = graphics::DlssMode::Balanced;
        } else if (value == "performance") {
            options->graphics.dlssMode = graphics::DlssMode::Performance;
        } else if (value == "ultraperformance") {
            options->graphics.dlssMode = graphics::DlssMode::UltraPerformance;
        } else {
            throw std::invalid_argument(
                "Unknown DLSS mode '" + value +
                "'; expected dlaa, quality, balanced, performance or ultraperformance");
        }
    }
    options->graphics.textureQuality = static_cast<TextureQuality>(vars["texquality"].as<int>());
    options->graphics.shadowResolution = 1 << (10 + vars["shadowres"].as<int>());
    options->graphics.shadowOpacity = vars["shadowopacity"].as<float>();
    options->graphics.anisotropicFiltering = vars["anisofilter"].as<int>();
    options->graphics.drawDistance = vars["drawdist"].as<float>();
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
