/*
 * Copyright (c) 2020-2026 The reone project contributors
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

#include "reone/graphics/optionsregistry.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace reone {

namespace graphics {

namespace {

using CategoryOverride = GraphicsOptions::CategoryOverride;

std::string formatFloat(float value) {
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return stream.str();
}

[[noreturn]] void rejectValue(const std::string &name, const std::string &value,
                              const std::string &expected) {
    throw std::invalid_argument("Graphics option '" + name + "': cannot read '" +
                                value + "' as " + expected);
}

bool parseBool(const std::string &name, const std::string &value) {
    if (value == "1" || value == "true" || value == "on" || value == "yes")
        return true;
    if (value == "0" || value == "false" || value == "off" || value == "no")
        return false;
    rejectValue(name, value, "a boolean (0/1, true/false, on/off, yes/no)");
}

int parseInt(const std::string &name, const std::string &value) {
    try {
        size_t consumed = 0;
        const int result = std::stoi(value, &consumed);
        if (consumed != value.size())
            rejectValue(name, value, "an integer");
        return result;
    } catch (const std::invalid_argument &) {
        rejectValue(name, value, "an integer");
    } catch (const std::out_of_range &) {
        rejectValue(name, value, "an integer in range");
    }
}

float parseFloat(const std::string &name, const std::string &value) {
    try {
        size_t consumed = 0;
        const float result = std::stof(value, &consumed);
        if (consumed != value.size())
            rejectValue(name, value, "a number");
        if (!std::isfinite(result))
            rejectValue(name, value, "a finite number");
        return result;
    } catch (const std::invalid_argument &) {
        rejectValue(name, value, "a number");
    } catch (const std::out_of_range &) {
        rejectValue(name, value, "a number in range");
    }
}

GraphicsOptionDesc boolOpt(const char *name, OptionApply apply, const char *help,
                           bool GraphicsOptions::*member) {
    GraphicsOptionDesc desc;
    desc.name = name;
    desc.apply = apply;
    desc.help = help;
    desc.get = [member](const GraphicsOptions &o) { return o.*member ? std::string("1") : std::string("0"); };
    desc.set = [member, name](GraphicsOptions &o, const std::string &value) {
        o.*member = parseBool(name, value);
    };
    desc.equal = [member](const GraphicsOptions &a, const GraphicsOptions &b) { return a.*member == b.*member; };
    desc.copy = [member](const GraphicsOptions &from, GraphicsOptions &to) { to.*member = from.*member; };
    return desc;
}

/**
 * Clamped exactly as optionsparser.cpp clamps the same flag, so a value that
 * arrives through the console lands where the same value on the command line
 * would have. Silently narrowing rather than rejecting is deliberate and
 * matches the parser; only unreadable text is an error.
 */
GraphicsOptionDesc intOpt(const char *name, OptionApply apply, const char *help,
                          int GraphicsOptions::*member, int low, int high) {
    GraphicsOptionDesc desc;
    desc.name = name;
    desc.apply = apply;
    desc.help = help;
    desc.get = [member](const GraphicsOptions &o) { return std::to_string(o.*member); };
    desc.set = [member, name, low, high](GraphicsOptions &o, const std::string &value) {
        o.*member = std::clamp(parseInt(name, value), low, high);
    };
    desc.equal = [member](const GraphicsOptions &a, const GraphicsOptions &b) { return a.*member == b.*member; };
    desc.copy = [member](const GraphicsOptions &from, GraphicsOptions &to) { to.*member = from.*member; };
    return desc;
}

GraphicsOptionDesc floatOpt(const char *name, OptionApply apply, const char *help,
                            float GraphicsOptions::*member, float low, float high) {
    GraphicsOptionDesc desc;
    desc.name = name;
    desc.apply = apply;
    desc.help = help;
    desc.get = [member](const GraphicsOptions &o) { return formatFloat(o.*member); };
    desc.set = [member, name, low, high](GraphicsOptions &o, const std::string &value) {
        o.*member = std::clamp(parseFloat(name, value), low, high);
    };
    desc.equal = [member](const GraphicsOptions &a, const GraphicsOptions &b) { return a.*member == b.*member; };
    desc.copy = [member](const GraphicsOptions &from, GraphicsOptions &to) { to.*member = from.*member; };
    return desc;
}

GraphicsOptionDesc catFloatOpt(const std::string &name, int index,
                               float CategoryOverride::*member, float low, float high) {
    GraphicsOptionDesc desc;
    desc.name = name;
    desc.apply = OptionApply::Live;
    desc.help = "material category " + std::to_string(index) + " override";
    desc.get = [index, member](const GraphicsOptions &o) {
        return formatFloat(o.categoryOverrides[index].*member);
    };
    desc.set = [index, member, name, low, high](GraphicsOptions &o, const std::string &value) {
        o.categoryOverrides[index].*member = std::clamp(parseFloat(name, value), low, high);
    };
    desc.equal = [index, member](const GraphicsOptions &a, const GraphicsOptions &b) {
        return a.categoryOverrides[index].*member == b.categoryOverrides[index].*member;
    };
    desc.copy = [index, member](const GraphicsOptions &from, GraphicsOptions &to) {
        to.categoryOverrides[index].*member = from.categoryOverrides[index].*member;
    };
    return desc;
}

GraphicsOptionDesc catColorOpt(const std::string &name, int index, int component) {
    GraphicsOptionDesc desc;
    desc.name = name;
    desc.apply = OptionApply::Live;
    desc.help = "material category " + std::to_string(index) + " override colour";
    desc.get = [index, component](const GraphicsOptions &o) {
        return formatFloat(o.categoryOverrides[index].color[component]);
    };
    desc.set = [index, component, name](GraphicsOptions &o, const std::string &value) {
        o.categoryOverrides[index].color[component] = parseFloat(name, value);
    };
    desc.equal = [index, component](const GraphicsOptions &a, const GraphicsOptions &b) {
        return a.categoryOverrides[index].color[component] ==
               b.categoryOverrides[index].color[component];
    };
    desc.copy = [index, component](const GraphicsOptions &from, GraphicsOptions &to) {
        to.categoryOverrides[index].color[component] =
            from.categoryOverrides[index].color[component];
    };
    return desc;
}

/** A named-value option, with the spellings optionsparser.cpp accepts. */
GraphicsOptionDesc enumOpt(const char *name, OptionApply apply, const char *help,
                           std::function<std::string(const GraphicsOptions &)> get,
                           std::function<void(GraphicsOptions &, const std::string &)> set,
                           std::function<bool(const GraphicsOptions &, const GraphicsOptions &)> equal,
                           std::function<void(const GraphicsOptions &, GraphicsOptions &)> copy) {
    GraphicsOptionDesc desc;
    desc.name = name;
    desc.apply = apply;
    desc.help = help;
    desc.get = std::move(get);
    desc.set = std::move(set);
    desc.equal = std::move(equal);
    desc.copy = std::move(copy);
    return desc;
}

std::vector<GraphicsOptionDesc> buildDescs() {
    std::vector<GraphicsOptionDesc> descs;

    // Window and swapchain. Width, height and the window scale are consumed by
    // Window::resize and by the scene pipeline's target allocation, both of
    // which the rebuild path drives; fullscreen and headless are read once, in
    // Window::init, and nothing re-reads them.
    descs.push_back(intOpt("width", OptionApply::Reapply, "render width",
                           &GraphicsOptions::width, 1, 16384));
    descs.push_back(intOpt("height", OptionApply::Reapply, "render height",
                           &GraphicsOptions::height, 1, 16384));
    descs.push_back(intOpt("winscale", OptionApply::Reapply, "window scale, percent",
                           &GraphicsOptions::winScale, 1, 400));
    descs.push_back(boolOpt("fullscreen", OptionApply::Restart, "enable fullscreen",
                            &GraphicsOptions::fullscreen));
    descs.push_back(boolOpt("headless", OptionApply::Restart,
                            "never show the window; for scripted batch runs",
                            &GraphicsOptions::headless));
    descs.push_back(boolOpt("vsync", OptionApply::Reapply, "enable v-sync",
                            &GraphicsOptions::vsync));
    descs.push_back(floatOpt("guiscale", OptionApply::Reapply, "GUI layout scale",
                             &GraphicsOptions::guiScale,
                             std::numeric_limits<float>::min(), std::numeric_limits<float>::max()));
    descs.push_back(floatOpt("guitextscale", OptionApply::Reapply, "GUI text scale",
                             &GraphicsOptions::guiTextScale,
                             std::numeric_limits<float>::min(), std::numeric_limits<float>::max()));
    descs.push_back(floatOpt("guidialogtextscale", OptionApply::Reapply, "dialog text scale",
                             &GraphicsOptions::guiDialogTextScale,
                             std::numeric_limits<float>::min(), std::numeric_limits<float>::max()));
    descs.push_back(floatOpt("guiborderscale", OptionApply::Reapply, "GUI border scale",
                             &GraphicsOptions::guiBorderScale,
                             std::numeric_limits<float>::min(), std::numeric_limits<float>::max()));
    descs.push_back(floatOpt("guilistscale", OptionApply::Reapply, "GUI list row scale",
                             &GraphicsOptions::guiListScale,
                             std::numeric_limits<float>::min(), std::numeric_limits<float>::max()));

    // Scene content pushed every update.
    descs.push_back(boolOpt("grass", OptionApply::Live, "enable grass",
                            &GraphicsOptions::grass));
    descs.push_back(boolOpt("shadows", OptionApply::Live, "enable shadows",
                            &GraphicsOptions::shadows));
    descs.push_back(boolOpt("particles", OptionApply::Live, "enable emitter particles",
                            &GraphicsOptions::particles));
    descs.push_back(boolOpt("lensflares", OptionApply::Live, "draw light halo billboards",
                            &GraphicsOptions::lensFlares));
    descs.push_back(floatOpt("thintransmission", OptionApply::Live,
                             "light a thin surface passes to its far side",
                             &GraphicsOptions::thinTransmission, 0.0f, 1.0f));
    descs.push_back(floatOpt("ptrefraction", OptionApply::Live,
                             "bend of a traced ray through a transparent surface",
                             &GraphicsOptions::ptRefraction, 0.0f, 1.0f));
    descs.push_back(floatOpt("grassdensity", OptionApply::Live, "grass density multiplier",
                             &GraphicsOptions::grassDensity, 0.0f, 64.0f));
    descs.push_back(enumOpt(
        "grasscardshape", OptionApply::Live, "fitted grass card outline",
        [](const GraphicsOptions &o) { return grassCardShapeName(o.grassCardShape); },
        [](GraphicsOptions &o, const std::string &value) { o.grassCardShape = parseGrassCardShape(value); },
        [](const GraphicsOptions &a, const GraphicsOptions &b) { return a.grassCardShape == b.grassCardShape; },
        [](const GraphicsOptions &from, GraphicsOptions &to) { to.grassCardShape = from.grassCardShape; }));
    descs.push_back(intOpt("grasscardsides", OptionApply::Live, "sides of a fitted grass k-gon",
                           &GraphicsOptions::grassCardSides, 3, 16));
    descs.push_back(intOpt("grasscardgrid", OptionApply::Live, "cells across a fitted grass grid",
                           &GraphicsOptions::grassCardGrid, 2, 32));
    descs.push_back(floatOpt("grasscardaspect", OptionApply::Live, "grass card width over length",
                             &GraphicsOptions::grassCardAspect, 0.1f, 4.0f));

    // What the renderer is, as one three-way choice.
    //
    // Its class is per VALUE, not per option. Retro and PBR pick a resolve step
    // per frame over targets a raster pipeline has already allocated, so moving
    // between them is live and must stay live. Path tracing decides whether the
    // ray-query pipeline exists and what format the scene output carries, both
    // fixed at construction, so crossing into or out of it takes a rebuild.
    descs.push_back(enumOpt(
        "mode", OptionApply::Reapply,
        "render mode: retro, pbr or path-tracing ('raster' is accepted for retro)",
        [](const GraphicsOptions &o) -> std::string { return renderModeName(o.mode); },
        [](GraphicsOptions &o, const std::string &value) {
            try {
                o.mode = parseRenderMode(value);
            } catch (const std::invalid_argument &) {
                throw std::invalid_argument(
                    "Graphics option 'mode': unknown render mode '" + value +
                    "'; expected retro, pbr or path-tracing");
            }
        },
        [](const GraphicsOptions &a, const GraphicsOptions &b) { return a.mode == b.mode; },
        [](const GraphicsOptions &from, GraphicsOptions &to) { to.mode = from.mode; }));
    descs.back().applyFor = [](const GraphicsOptions &a, const GraphicsOptions &b) {
        return a.mode == RenderMode::PathTracing || b.mode == RenderMode::PathTracing
                   ? OptionApply::Reapply
                   : OptionApply::Live;
    };

    descs.push_back(boolOpt("admissionshadow", OptionApply::Live,
                            "compare incremental and full scene admission every frame",
                            &GraphicsOptions::admissionShadow));
    descs.push_back(boolOpt("admissionforcefull", OptionApply::Live,
                            "force full scene collection and classification",
                            &GraphicsOptions::admissionForceFull));

    // Path tracing dials: all of them ride in push constants or in NRD's
    // per-dispatch tuning struct, so the next traced frame reads them.
    descs.push_back(intOpt("ptspp", OptionApply::Live, "path tracing samples per pixel",
                           &GraphicsOptions::pathTracingSamples, 1, 64));
    descs.push_back(floatOpt("skyintensity", OptionApply::Live, "sky light intensity",
                             &GraphicsOptions::skyIntensity, 0.0f, 1024.0f));
    descs.push_back(floatOpt("ptemissiveintensity", OptionApply::Live,
                             "path tracing emissive intensity",
                             &GraphicsOptions::ptEmissiveIntensity, 0.0f, 1024.0f));
    descs.push_back(floatOpt("ptbackdropintensity", OptionApply::Live,
                             "path tracing backdrop imagery intensity",
                             &GraphicsOptions::ptBackdropIntensity, 0.0f, 1024.0f));
    descs.push_back(floatOpt("ptlightmapintensity", OptionApply::Live,
                             "path tracing lightmap intensity",
                             &GraphicsOptions::ptLightmapIntensity, 0.0f, 1024.0f));
    descs.push_back(floatOpt("ptdirectintensity", OptionApply::Live,
                             "path tracing direct-light intensity",
                             &GraphicsOptions::ptDirectIntensity, 0.0f, 1024.0f));
    descs.push_back(floatOpt("ptsunintensity", OptionApply::Live, "path tracing sun intensity",
                             &GraphicsOptions::ptSunIntensity, 0.0f, 1024.0f));
    descs.push_back(intOpt("ptbounces", OptionApply::Live, "path tracing bounces",
                           &GraphicsOptions::ptBounces, 1, 8));
    descs.push_back(floatOpt("ptrayoffset", OptionApply::Live, "path tracing ray origin offset",
                             &GraphicsOptions::ptRayOffset, 0.0001f, 1.0f));
    descs.push_back(floatOpt("albedogamma", OptionApply::Live,
                             "authored albedo decode exponent, PBR and path tracing alike "
                             "(2.2 is sRGB-correct, 1.0 matches the reference engines)",
                             &GraphicsOptions::albedoGamma, 0.1f, 4.0f));
    descs.push_back(floatOpt("emissivegamma", OptionApply::Live,
                             "authored radiance decode exponent (emission, sky, backdrop)",
                             &GraphicsOptions::emissiveGamma, 0.1f, 4.0f));
    descs.push_back(floatOpt("pbrlightmapintensity", OptionApply::Live,
                             "PBR baked-irradiance intensity",
                             &GraphicsOptions::pbrLightmapIntensity, 0.0f, 1024.0f));
    descs.push_back(boolOpt("pttracestats", OptionApply::Live,
                            "enable path tracing statistics",
                            &GraphicsOptions::ptTraceStats));
    descs.push_back(intOpt("tonemap", OptionApply::Live, "display transform: 0 off, 1 Gran Turismo curve",
                           &GraphicsOptions::tonemap, 0, 1));
    descs.push_back(floatOpt("exposure", OptionApply::Live,
                             "scene-referred exposure ahead of the tonemap",
                             &GraphicsOptions::exposure, 0.05f, 64.0f));
    descs.push_back(floatOpt("ptpointemitterratio", OptionApply::Live,
                             "point-light emitter radius, as a fraction of influence radius",
                             &GraphicsOptions::ptPointEmitterRatio, 0.01f, 0.5f));
    descs.push_back(floatOpt("ptbounceroughness", OptionApply::Live,
                             "roughness floor after the first scatter",
                             &GraphicsOptions::ptBounceRoughness, 0.0f, 1.0f));
    descs.push_back(floatOpt("ptroughnessfloor", OptionApply::Live,
                             "lowest roughness any surface may take",
                             &GraphicsOptions::ptRoughnessFloor, 0.0f, 1.0f));
    descs.push_back(floatOpt("ptindirectclamp", OptionApply::Live,
                             "ceiling on one indirect sample, 0 disables",
                             &GraphicsOptions::ptIndirectClamp, 0.0f, 64.0f));
    descs.push_back(floatOpt("ptsunangularsize", OptionApply::Live,
                             "path tracing sun angular size, degrees",
                             &GraphicsOptions::ptSunAngularSize, 0.05f, 90.0f));

    // Read while the shared material records are built. The admission layer
    // caches classification, so its options fingerprint has to cover this or
    // the toggle would sit inert behind the cache - see admission.cpp.
    descs.push_back(intOpt("maxlights", OptionApply::Live,
                           "lights a frame may carry",
                           &GraphicsOptions::maxLights, 1, kMaxLights));
    // Reapply, not Live: these size the shadow images the pipeline allocates,
    // so changing one has to rebuild the pipeline rather than take effect on
    // the next frame against arrays that are the old size. Retro ignores both
    // and uses the original's combined figure - see SceneGraph::shadowBudget.
    descs.push_back(intOpt("maxdirectionalshadows", OptionApply::Reapply,
                           "shadow-casting directional lights (PBR and path tracing)",
                           &GraphicsOptions::maxDirectionalShadows, 0, 4));
    descs.push_back(intOpt("maxpointshadows", OptionApply::Reapply,
                           "shadow-casting point lights (PBR and path tracing)",
                           &GraphicsOptions::maxPointShadows, 0, kMaxPointShadows));
    descs.push_back(intOpt("pointshadowres", OptionApply::Reapply,
                           "point shadow cube face resolution",
                           &GraphicsOptions::pointShadowResolution, 128, 2048));
    descs.push_back(boolOpt("lightmaps", OptionApply::Live,
                            "apply lightmaps (diagnostic toggle)",
                            &GraphicsOptions::lightmaps));

    // Both gate work inside the PBR mode, and both are read where the frame is
    // planned or a push constant is filled, so the next frame follows. Off,
    // neither costs anything: the occlusion branch is not taken and the
    // reflection step is not appended.
    descs.push_back(boolOpt("ssao", OptionApply::Live,
                            "screen-space ambient occlusion, inside the PBR resolve",
                            &GraphicsOptions::ssao));
    descs.push_back(boolOpt("ssr", OptionApply::Live,
                            "screen-space reflections over the PBR resolve",
                            &GraphicsOptions::ssr));

    // The temporal occupant of the slot owns device images of its own, built in
    // ScenePipeline::init, so the choice is fixed for the life of the targets.
    descs.push_back(enumOpt(
        "antialiasing", OptionApply::Reapply,
        "anti-aliasing in the common slot: off, fxaa or fsr",
        [](const GraphicsOptions &o) -> std::string {
            switch (o.antialiasing) {
            case AntiAliasing::Fxaa:
                return "fxaa";
            case AntiAliasing::Fsr:
                return "fsr";
            default:
                return "off";
            }
        },
        [](GraphicsOptions &o, const std::string &value) {
            if (value == "off" || value == "none") {
                o.antialiasing = AntiAliasing::None;
            } else if (value == "fxaa") {
                o.antialiasing = AntiAliasing::Fxaa;
            } else if (value == "fsr") {
                o.antialiasing = AntiAliasing::Fsr;
            } else {
                throw std::invalid_argument(
                    "Graphics option 'antialiasing': unknown mode '" + value +
                    "'; expected off, fxaa or fsr");
            }
        },
        [](const GraphicsOptions &a, const GraphicsOptions &b) {
            return a.antialiasing == b.antialiasing;
        },
        [](const GraphicsOptions &from, GraphicsOptions &to) {
            to.antialiasing = from.antialiasing;
        }));

    descs.push_back(boolOpt("bloom", OptionApply::Live, "bloom emissive highlights",
                            &GraphicsOptions::bloom));
    descs.push_back(floatOpt("bloomthreshold", OptionApply::Live,
                             "level an emissive texel must pass to bloom",
                             &GraphicsOptions::bloomThreshold, 0.0f,
                             std::numeric_limits<float>::max()));
    descs.push_back(floatOpt("bloomintensity", OptionApply::Live,
                             "scale on what bloom adds back",
                             &GraphicsOptions::bloomIntensity, 0.0f,
                             std::numeric_limits<float>::max()));
    descs.push_back(boolOpt("grade", OptionApply::Live,
                            "apply exposure and the tone curve; the display "
                            "transform itself is never optional",
                            &GraphicsOptions::grade));
    descs.push_back(boolOpt("sharpen", OptionApply::Live,
                            "sharpen the finished frame, after the display transform",
                            &GraphicsOptions::sharpen));
    descs.push_back(floatOpt("sharpenamount", OptionApply::Live,
                             "strength of the sharpen mask",
                             &GraphicsOptions::sharpenAmount, 0.0f, 4.0f));

    // Common, not traced-only: the channels are G-buffer quantities and every
    // mode draws that G-buffer. Sits here with the other frame-wide dials
    // rather than in the path tracing block it used to belong to.
    descs.push_back(intOpt("debugview", OptionApply::Live,
                           "debug channel view in any render mode, 0 off",
                           &GraphicsOptions::debugView, 0, kMaxDebugView));
    // Decided at the point of use, in SceneGraph::computeJitter, so it follows
    // the active resolver on the next frame with nothing rebuilt.

    // The denoiser is built whenever the build carries NRD, regardless of this
    // dial; the dial only selects whether its composite overwrites the trace.
    descs.push_back(boolOpt("ptdenoise", OptionApply::Live,
                            "enable the path tracing denoiser",
                            &GraphicsOptions::ptDenoise));
    descs.push_back(enumOpt(
        "ptshadowfilter", OptionApply::Live,
        "what settles the direct channel: off, penumbra or denoiser",
        [](const GraphicsOptions &o) -> std::string {
            switch (o.ptShadowFilter) {
            case ShadowFilter::Penumbra: return "penumbra";
            case ShadowFilter::Denoiser: return "denoiser";
            default: return "off";
            }
        },
        [](GraphicsOptions &o, const std::string &value) {
            if (value == "off" || value == "none" || value == "0") {
                o.ptShadowFilter = ShadowFilter::Off;
            } else if (value == "penumbra" || value == "1") {
                o.ptShadowFilter = ShadowFilter::Penumbra;
            } else if (value == "denoiser" || value == "nrd") {
                o.ptShadowFilter = ShadowFilter::Denoiser;
            } else {
                throw std::invalid_argument(
                    "Graphics option 'ptshadowfilter': unknown mode '" + value +
                    "'; expected off, penumbra or denoiser");
            }
        },
        [](const GraphicsOptions &a, const GraphicsOptions &b) {
            return a.ptShadowFilter == b.ptShadowFilter;
        },
        [](const GraphicsOptions &from, GraphicsOptions &to) {
            to.ptShadowFilter = from.ptShadowFilter;
        }));
    descs.push_back(floatOpt("ptshadowfiltermaxradius", OptionApply::Live,
                             "ceiling on the shadow filter radius, pixels",
                             &GraphicsOptions::ptShadowFilterMaxRadius, 1.0f, 64.0f));
    // Grass shape. All live: the merge kernel reads them from a push constant,
    // so a change costs a dispatch rather than a rebuild of the face records.
    descs.push_back(floatOpt("grassradius", OptionApply::Live,
                             "grass draw radius, world units",
                             &GraphicsOptions::grassRadius, 0.0f, 512.0f));
    descs.push_back(floatOpt("grasswindstrength", OptionApply::Live,
                             "extra bend at full gust, radians; 0 is still air",
                             &GraphicsOptions::grassWindStrength, 0.0f, 2.0f));
    descs.push_back(floatOpt("grasswinddirection", OptionApply::Live,
                             "wind direction, radians",
                             &GraphicsOptions::grassWindDirection, -6.2832f, 6.2832f));
    descs.push_back(floatOpt("grasswindspeed", OptionApply::Live,
                             "how fast the rustle travels",
                             &GraphicsOptions::grassWindSpeed, 0.0f, 10.0f));
    descs.push_back(floatOpt("grasswindwavelength", OptionApply::Live,
                             "distance between wind crests, world units",
                             &GraphicsOptions::grassWindWavelength, 0.1f, 64.0f));
    descs.push_back(floatOpt("grasswindgust", OptionApply::Live,
                             "share of the strength carried by the slow gust",
                             &GraphicsOptions::grassWindGust, 0.0f, 1.0f));
    descs.push_back(floatOpt("grassorientation", OptionApply::Live,
                             "which way a blade faces, radians",
                             &GraphicsOptions::grassOrientation, -6.2832f, 6.2832f));
    descs.push_back(floatOpt("grassorientationvariance", OptionApply::Live,
                             "how far a blade may stray from that, radians; 0 aligns them all",
                             &GraphicsOptions::grassOrientationVariance, 0.0f, 6.2832f));
    descs.push_back(floatOpt("grasscurvature", OptionApply::Live,
                             "total bend of a blade, radians",
                             &GraphicsOptions::grassCurvature, -2.0f, 2.0f));
    descs.push_back(floatOpt("grasscurvaturevariance", OptionApply::Live,
                             "how much that bend varies between blades",
                             &GraphicsOptions::grassCurvatureVariance, 0.0f, 2.0f));
    descs.push_back(floatOpt("grasssparsity", OptionApply::Live,
                             "fraction of slots left empty, which buys clumping",
                             &GraphicsOptions::grassSparsity, 0.0f, 0.99f));
    descs.push_back(floatOpt("grassdisplacement", OptionApply::Live,
                             "displacement from the slot centre, in cells",
                             &GraphicsOptions::grassDisplacement, 0.0f, 3.0f));
    descs.push_back(floatOpt("grasslength", OptionApply::Live,
                             "blade length, times the area's authored quad size",
                             &GraphicsOptions::grassLength, 0.0f, 8.0f));
    descs.push_back(floatOpt("grasslengthvariance", OptionApply::Live,
                             "how much that length varies between blades",
                             &GraphicsOptions::grassLengthVariance, 0.0f, 1.0f));
    descs.push_back(floatOpt("grasswidth", OptionApply::Live,
                             "blade width as a fraction of its length",
                             &GraphicsOptions::grassWidth, 0.0f, 1.0f));
    descs.push_back(floatOpt("grassyoffset", OptionApply::Live,
                             "root offset as a fraction of length; negative sinks it",
                             &GraphicsOptions::grassYOffset, -1.0f, 1.0f));
    // A vector through the same string channel every other option uses, so it
    // saves, parses and reaches the console without a second mechanism.
    descs.push_back(enumOpt(
        "grasscolor", OptionApply::Live,
        "blade albedo as \"r g b\"",
        [](const GraphicsOptions &o) -> std::string {
            std::ostringstream stream;
            stream << o.grassColor.r << " " << o.grassColor.g << " " << o.grassColor.b;
            return stream.str();
        },
        [](GraphicsOptions &o, const std::string &value) {
            std::istringstream stream(value);
            glm::vec3 parsed {0.0f};
            if (!(stream >> parsed.r >> parsed.g >> parsed.b)) {
                throw std::invalid_argument(
                    "Graphics option 'grasscolor': expected three numbers, got '" + value + "'");
            }
            o.grassColor = glm::max(parsed, glm::vec3(0.0f));
        },
        [](const GraphicsOptions &a, const GraphicsOptions &b) {
            return a.grassColor == b.grassColor;
        },
        [](const GraphicsOptions &from, GraphicsOptions &to) { to.grassColor = from.grassColor; }));
    descs.push_back(floatOpt("grassroughness", OptionApply::Live,
                             "blade roughness, set outright rather than derived",
                             &GraphicsOptions::grassRoughness, 0.0f, 1.0f));
    descs.push_back(intOpt("grassbladespercluster", OptionApply::Live,
                           "blades grown per authored cluster",
                           &GraphicsOptions::grassBladesPerCluster, 1, 32));
    descs.push_back(intOpt("grasstrianglebudget", OptionApply::Live,
                           "ceiling on grass triangles in the scene",
                           &GraphicsOptions::grassTriangleBudget, 0, kMaxGrassTriangleBudget));
    descs.push_back(floatOpt("ptshadowfilterscale", OptionApply::Live,
                             "multiplier on the radius the geometry implies",
                             &GraphicsOptions::ptShadowFilterRadiusScale, 0.0f, 8.0f));
    descs.push_back(floatOpt("ptshadowfilterminradius", OptionApply::Live,
                             "floor on the shadow filter radius where light is blocked, pixels",
                             &GraphicsOptions::ptShadowFilterMinRadius, 0.0f, 32.0f));
    descs.push_back(floatOpt("ptshadowfilterdepthtolerance", OptionApply::Live,
                             "relative view-depth difference a filter tap may have",
                             &GraphicsOptions::ptShadowFilterDepthTolerance, 0.0f, 1.0f));
    descs.push_back(floatOpt("ptshadowfilternormaltolerance", OptionApply::Live,
                             "minimum normal agreement a filter tap may have",
                             &GraphicsOptions::ptShadowFilterNormalTolerance, -1.0f, 1.0f));
    descs.push_back(boolOpt("ptdirectchannel", OptionApply::Live,
                            "apply primary-vertex direct light at the resolve "
                            "instead of through the denoiser",
                            &GraphicsOptions::ptDirectChannel));
    // Seconds, not frames - see TracingDenoiserTuning.
    descs.push_back(floatOpt("ptnrdaccumtime", OptionApply::Live,
                             "denoiser history, seconds",
                             &GraphicsOptions::ptNrdAccumulationTime, 0.0f, 2.0f));
    descs.push_back(floatOpt("ptnrdfastaccumtime", OptionApply::Live,
                             "denoiser responsive history, seconds",
                             &GraphicsOptions::ptNrdFastAccumulationTime, 0.0f, 2.0f));
    descs.push_back(intOpt("ptnrdhistoryfix", OptionApply::Live, "denoiser history fix frames",
                           &GraphicsOptions::ptNrdHistoryFixFrames, 0, 63));
    descs.push_back(floatOpt("ptnrddirectaccumtime", OptionApply::Live,
                             "direct-light denoiser history, seconds",
                             &GraphicsOptions::ptNrdDirectAccumulationTime, 0.0f, 2.0f));
    descs.push_back(intOpt("ptnrddirectatrous", OptionApply::Live,
                           "direct-light denoiser A-trous iterations",
                           &GraphicsOptions::ptNrdDirectAtrousIterations, 2, 8));
    descs.push_back(floatOpt("ptnrddirectphiluminance", OptionApply::Live,
                             "direct-light denoiser luminance edge stopping",
                             &GraphicsOptions::ptNrdDirectPhiLuminance, 0.0f, 16.0f));
    descs.push_back(floatOpt("ptnrddiffuseprepassblurradius", OptionApply::Live,
                             "diffuse prepass blur radius",
                             &GraphicsOptions::ptNrdDiffusePrepassBlurRadius, 0.0f, 256.0f));
    descs.push_back(floatOpt("ptnrdspecularprepassblurradius", OptionApply::Live,
                             "specular prepass blur radius",
                             &GraphicsOptions::ptNrdSpecularPrepassBlurRadius, 0.0f, 256.0f));
    descs.push_back(floatOpt("ptnrdlobeanglefraction", OptionApply::Live,
                             "lobe angle fraction: normal-based history rejection",
                             &GraphicsOptions::ptNrdLobeAngleFraction, 0.01f, 1.0f));
    descs.push_back(floatOpt("ptnrdroughnessfraction", OptionApply::Live,
                             "roughness fraction: roughness-based history rejection",
                             &GraphicsOptions::ptNrdRoughnessFraction, 0.01f, 1.0f));
    descs.push_back(floatOpt("ptnrddisocclusionthreshold", OptionApply::Live,
                             "disocclusion threshold",
                             &GraphicsOptions::ptNrdDisocclusionThreshold, 0.0f, 16.0f));
    descs.push_back(boolOpt("ptnrdantifirefly", OptionApply::Live, "enable anti-firefly",
                            &GraphicsOptions::ptNrdAntiFirefly));
    descs.push_back(intOpt("ptnrdatrous", OptionApply::Live, "RELAX a-trous iterations",
                           &GraphicsOptions::ptNrdAtrousIterations, 2, 8));
    descs.push_back(floatOpt("ptnrddiffusephi", OptionApply::Live,
                             "RELAX diffuse luminance edge stopper",
                             &GraphicsOptions::ptNrdDiffusePhiLuminance, 0.0f, 16.0f));
    descs.push_back(floatOpt("ptnrdspecularphi", OptionApply::Live,
                             "RELAX specular luminance edge stopper",
                             &GraphicsOptions::ptNrdSpecularPhiLuminance, 0.0f, 16.0f));
    descs.push_back(floatOpt("ptnrddepththreshold", OptionApply::Live,
                             "RELAX depth threshold for spatial passes",
                             &GraphicsOptions::ptNrdDepthThreshold, 0.0f, 1.0f));
    descs.push_back(floatOpt("ptnrdspecularlobeslack", OptionApply::Live,
                             "RELAX specular lobe angle slack, degrees",
                             &GraphicsOptions::ptNrdSpecularLobeAngleSlack, 0.0f, 4.0f));

    // Inert unless the slot runs FSR, but read per dispatch either way.
    // Reapply, not Live: it decides what the pipeline allocates.
    descs.push_back(floatOpt("renderscale", OptionApply::Reapply,
                             "trace and raster at this fraction of display resolution; FSR upscales",
                             &GraphicsOptions::renderScale, 0.25f, 1.0f));
    descs.push_back(floatOpt("fsrsharpness", OptionApply::Live,
                             "FSR RCAS sharpening, 0 disables the pass",
                             &GraphicsOptions::fsrSharpness, 0.0f, 1.0f));

    // Which texture pack the resource director mounts, decided once while the
    // game directory is opened.
    descs.push_back(enumOpt(
        "texquality", OptionApply::Restart, "texture quality: 0 high, 1 medium, 2 low",
        [](const GraphicsOptions &o) {
            return std::to_string(static_cast<int>(o.textureQuality));
        },
        [](GraphicsOptions &o, const std::string &value) {
            o.textureQuality = static_cast<TextureQuality>(
                std::clamp(parseInt("texquality", value), 0, 2));
        },
        [](const GraphicsOptions &a, const GraphicsOptions &b) {
            return a.textureQuality == b.textureQuality;
        },
        [](const GraphicsOptions &from, GraphicsOptions &to) {
            to.textureQuality = from.textureQuality;
        }));

    // Written as the command line writes it - an exponent above 1024 - so that
    // "shadowres 2" means the same thing everywhere.
    descs.push_back(enumOpt(
        "shadowres", OptionApply::Reapply,
        "shadow map resolution as an exponent above 1024: 0 gives 1024, 3 gives 8192",
        [](const GraphicsOptions &o) {
            int exponent = 0;
            while ((1 << (10 + exponent)) < o.shadowResolution && exponent < 4)
                ++exponent;
            return std::to_string(exponent);
        },
        [](GraphicsOptions &o, const std::string &value) {
            o.shadowResolution = 1 << (10 + std::clamp(parseInt("shadowres", value), 0, 3));
        },
        [](const GraphicsOptions &a, const GraphicsOptions &b) {
            return a.shadowResolution == b.shadowResolution;
        },
        [](const GraphicsOptions &from, GraphicsOptions &to) {
            to.shadowResolution = from.shadowResolution;
        }));

    // Read into the globals block every frame; negative keeps what the ARE
    // authored, which is why the lower bound is below zero.
    descs.push_back(floatOpt("shadowopacity", OptionApply::Live,
                             "override the module's authored shadow opacity (0-1; <0 keeps authored)",
                             &GraphicsOptions::shadowOpacity, -1.0f, 1.0f));

    // Baked into each Texture's sampler while it is loaded.
    descs.push_back(intOpt("anisofilter", OptionApply::Restart,
                           "anisotropic filtering, as a log2 sample count",
                           &GraphicsOptions::anisotropicFiltering, 0, 4));

    // Pushed onto model scene nodes as they are created. Nothing reads it back
    // today, so a change is inert either way; classified Live because that is
    // what it would be if a consumer returned.
    descs.push_back(floatOpt("drawdist", OptionApply::Live,
                             "draw distance (currently unread)",
                             &GraphicsOptions::drawDistance, 1.0f, 100000.0f));

    for (int i = 0; i < 9; ++i) {
        const auto key = "cat" + std::to_string(i);
        descs.push_back(catColorOpt(key + "color0", i, 0));
        descs.push_back(catColorOpt(key + "color1", i, 1));
        descs.push_back(catColorOpt(key + "color2", i, 2));
        descs.push_back(catFloatOpt(key + "colorweight", i, &CategoryOverride::colorWeight, 0.0f, 1.0f));
        // Negative means "no override", so the floor is below zero.
        descs.push_back(catFloatOpt(key + "roughness", i, &CategoryOverride::roughness, -1.0f, 1.0f));
        descs.push_back(catFloatOpt(key + "roughnessscale", i, &CategoryOverride::roughnessScale, 0.0f, 64.0f));
        descs.push_back(catFloatOpt(key + "emission", i, &CategoryOverride::emissionScale, 0.0f, 64.0f));
        descs.push_back(catFloatOpt(key + "env", i, &CategoryOverride::envScale, 0.0f, 64.0f));
        descs.push_back(catFloatOpt(key + "metallic", i, &CategoryOverride::metallicScale, 0.0f, 64.0f));
    }

    return descs;
}

} // namespace

const char *renderModeName(RenderMode mode) {
    switch (mode) {
    case RenderMode::Retro:
        return "retro";
    case RenderMode::PathTracing:
        return "path-tracing";
    default:
        return "pbr";
    }
}

RenderMode parseRenderMode(const std::string &value) {
    if (value == "retro" || value == "raster")
        return RenderMode::Retro;
    if (value == "pbr")
        return RenderMode::PBR;
    if (value == "path-tracing")
        return RenderMode::PathTracing;
    throw std::invalid_argument("Unknown render mode '" + value +
                                "'; expected retro, pbr or path-tracing");
}

const char *optionApplyName(OptionApply apply) {
    switch (apply) {
    case OptionApply::Reapply:
        return "requires reapply";
    case OptionApply::Restart:
        return "requires restart";
    default:
        return "live";
    }
}

const std::vector<GraphicsOptionDesc> &graphicsOptionDescs() {
    static const std::vector<GraphicsOptionDesc> descs = buildDescs();
    return descs;
}

/**
 * Names an option answers to that are not its own.
 *
 * `post` used to switch the post-process pass on and off, back when that pass
 * was an effect rather than the frame's encode. The dial that replaced it
 * grades the frame, so it is spelled `grade`.
 *
 * `ptdebugview` selected the path tracer's own channel views, back when only
 * the traced mode honoured them. The same numbers now select the same channels
 * in every mode, so the dial has dropped the prefix.
 *
 * In both cases scripts and commands files written against the old name keep
 * working and land on the option that inherited the meaning. Aliases are
 * resolved on lookup only, so `gfx list` still names every option exactly once.
 */
static const std::unordered_map<std::string, std::string> &optionAliases() {
    static const std::unordered_map<std::string, std::string> aliases {
        {"post", "grade"},
        {"ptdebugview", "debugview"}};
    return aliases;
}

const GraphicsOptionDesc *findGraphicsOptionDesc(const std::string &name) {
    static const std::unordered_map<std::string, const GraphicsOptionDesc *> byName = []() {
        std::unordered_map<std::string, const GraphicsOptionDesc *> result;
        for (const auto &desc : graphicsOptionDescs())
            result.emplace(desc.name, &desc);
        for (const auto &[alias, target] : optionAliases()) {
            const auto found = result.find(target);
            if (found != result.end())
                result.emplace(alias, found->second);
        }
        return result;
    }();
    const auto found = byName.find(name);
    return found != byName.end() ? found->second : nullptr;
}

OptionApply graphicsOptionApply(const GraphicsOptionDesc &desc,
                                const GraphicsOptions &left,
                                const GraphicsOptions &right) {
    return desc.applyFor ? desc.applyFor(left, right) : desc.apply;
}

std::vector<std::string> graphicsOptionsDiffering(const GraphicsOptions &left,
                                                  const GraphicsOptions &right,
                                                  OptionApply apply) {
    std::vector<std::string> names;
    for (const auto &desc : graphicsOptionDescs()) {
        if (graphicsOptionApply(desc, left, right) == apply && !desc.equal(left, right))
            names.push_back(desc.name);
    }
    return names;
}

void copyGraphicsOptions(const GraphicsOptions &from, GraphicsOptions &to,
                         OptionApply apply) {
    for (const auto &desc : graphicsOptionDescs()) {
        // Classified against the pair being reconciled, so a value-dependent
        // option is copied by the same rule that listed it as differing. Asking
        // desc.apply here instead would carry a live mode change into a rebuild
        // batch, or leave a rebuilding one behind.
        if (graphicsOptionApply(desc, from, to) == apply)
            desc.copy(from, to);
    }
}

} // namespace graphics

} // namespace reone
