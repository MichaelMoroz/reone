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

    // Scene content pushed every update.
    descs.push_back(boolOpt("grass", OptionApply::Live, "enable grass",
                            &GraphicsOptions::grass));
    descs.push_back(floatOpt("grassdensity", OptionApply::Live, "grass density multiplier",
                             &GraphicsOptions::grassDensity, 0.0f, 8.0f));

    // The resolve step and the shadow-caster policy are chosen per frame in
    // RenderPipeline::render, and a raster pipeline builds both resolve
    // descriptor sets, so this switches without rebuilding anything.
    descs.push_back(boolOpt("pbr", OptionApply::Live,
                            "physically-based raster resolve, rather than retro",
                            &GraphicsOptions::pbr));

    // What the pipeline is. Decided once, at construction: the ray-query
    // pipeline exists only in the traced mode, and the scene output format
    // follows from it.
    descs.push_back(enumOpt(
        "mode", OptionApply::Reapply, "render mode: raster or path-tracing",
        [](const GraphicsOptions &o) { return o.mode; },
        [](GraphicsOptions &o, const std::string &value) {
            if (value != "raster" && value != "path-tracing") {
                throw std::invalid_argument(
                    "Graphics option 'mode': unknown render mode '" + value +
                    "'; expected raster or path-tracing");
            }
            o.mode = value;
        },
        [](const GraphicsOptions &a, const GraphicsOptions &b) { return a.mode == b.mode; },
        [](const GraphicsOptions &from, GraphicsOptions &to) { to.mode = from.mode; }));

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
    descs.push_back(boolOpt("pttracestats", OptionApply::Live,
                            "enable path tracing statistics",
                            &GraphicsOptions::ptTraceStats));
    descs.push_back(intOpt("tonemap", OptionApply::Live, "display transform: 0 off, 1 ACES",
                           &GraphicsOptions::tonemap, 0, 1));
    descs.push_back(floatOpt("exposure", OptionApply::Live,
                             "scene-referred exposure ahead of the tonemap",
                             &GraphicsOptions::exposure, 0.05f, 64.0f));
    descs.push_back(floatOpt("ptpointemitterratio", OptionApply::Live,
                             "point-light emitter radius, as a fraction of influence radius",
                             &GraphicsOptions::ptPointEmitterRatio, 0.01f, 0.5f));
    descs.push_back(floatOpt("ptsunangularsize", OptionApply::Live,
                             "path tracing sun angular size, degrees",
                             &GraphicsOptions::ptSunAngularSize, 0.05f, 90.0f));

    // Read while the shared material records are built. The admission layer
    // caches classification, so its options fingerprint has to cover this or
    // the toggle would sit inert behind the cache - see admission.cpp.
    descs.push_back(boolOpt("lightmaps", OptionApply::Live,
                            "apply lightmaps (diagnostic toggle)",
                            &GraphicsOptions::lightmaps));

    // Nothing in the render path reads these two today; they are carried so the
    // command line, the config and the console agree on the vocabulary.
    descs.push_back(boolOpt("ssao", OptionApply::Live,
                            "enable screen-space ambient occlusion (currently unread)",
                            &GraphicsOptions::ssao));
    descs.push_back(boolOpt("ssr", OptionApply::Live,
                            "enable screen-space reflections (currently unread)",
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

    descs.push_back(boolOpt("post", OptionApply::Live, "enable the post-process pass",
                            &GraphicsOptions::post));
    descs.push_back(boolOpt("sharpen", OptionApply::Live,
                            "enable image sharpening (currently unread)",
                            &GraphicsOptions::sharpen));

    // Decided at the point of use, in SceneGraph::computeJitter, so it follows
    // the active resolver on the next frame with nothing rebuilt.
    descs.push_back(enumOpt(
        "taajitter", OptionApply::Live,
        "sub-pixel projection jitter: auto, on or off",
        [](const GraphicsOptions &o) -> std::string {
            switch (o.taaJitter) {
            case JitterMode::On:
                return "on";
            case JitterMode::Off:
                return "off";
            default:
                return "auto";
            }
        },
        [](GraphicsOptions &o, const std::string &value) {
            if (value == "auto") {
                o.taaJitter = JitterMode::Auto;
            } else if (value == "on" || value == "1") {
                o.taaJitter = JitterMode::On;
            } else if (value == "off" || value == "0") {
                o.taaJitter = JitterMode::Off;
            } else {
                throw std::invalid_argument(
                    "Graphics option 'taajitter': unknown mode '" + value +
                    "'; expected auto, on or off");
            }
        },
        [](const GraphicsOptions &a, const GraphicsOptions &b) {
            return a.taaJitter == b.taaJitter;
        },
        [](const GraphicsOptions &from, GraphicsOptions &to) { to.taaJitter = from.taaJitter; }));

    // The denoiser is built whenever the build carries NRD, regardless of this
    // dial; the dial only selects whether its composite overwrites the trace.
    descs.push_back(boolOpt("ptdenoise", OptionApply::Live,
                            "enable the path tracing denoiser",
                            &GraphicsOptions::ptDenoise));
    descs.push_back(intOpt("ptdebugview", OptionApply::Live, "path tracing debug view, 0 off",
                           &GraphicsOptions::ptDebugView, 0, 12));
    descs.push_back(intOpt("ptnrdstabilized", OptionApply::Live, "REBLUR stabilized frames",
                           &GraphicsOptions::ptNrdMaxStabilizedFrames, 0, 63));
    descs.push_back(intOpt("ptnrdaccum", OptionApply::Live, "REBLUR accumulated frames",
                           &GraphicsOptions::ptNrdMaxAccumulatedFrames, 0, 63));
    descs.push_back(intOpt("ptnrdfastaccum", OptionApply::Live, "REBLUR fast accumulated frames",
                           &GraphicsOptions::ptNrdMaxFastAccumulatedFrames, 0, 63));
    descs.push_back(intOpt("ptnrdhistoryfix", OptionApply::Live, "REBLUR history fix frames",
                           &GraphicsOptions::ptNrdHistoryFixFrames, 0, 63));
    descs.push_back(floatOpt("ptnrddiffuseprepassblurradius", OptionApply::Live,
                             "REBLUR diffuse prepass blur radius",
                             &GraphicsOptions::ptNrdDiffusePrepassBlurRadius, 0.0f, 256.0f));
    descs.push_back(floatOpt("ptnrdspecularprepassblurradius", OptionApply::Live,
                             "REBLUR specular prepass blur radius",
                             &GraphicsOptions::ptNrdSpecularPrepassBlurRadius, 0.0f, 256.0f));
    descs.push_back(floatOpt("ptnrdminblurradius", OptionApply::Live, "REBLUR minimum blur radius",
                             &GraphicsOptions::ptNrdMinBlurRadius, 0.0f, 256.0f));
    descs.push_back(floatOpt("ptnrdmaxblurradius", OptionApply::Live, "REBLUR maximum blur radius",
                             &GraphicsOptions::ptNrdMaxBlurRadius, 0.0f, 256.0f));
    descs.push_back(floatOpt("ptnrdlobeanglefraction", OptionApply::Live,
                             "REBLUR lobe angle fraction",
                             &GraphicsOptions::ptNrdLobeAngleFraction, 0.01f, 1.0f));
    descs.push_back(floatOpt("ptnrdroughnessfraction", OptionApply::Live,
                             "REBLUR roughness fraction",
                             &GraphicsOptions::ptNrdRoughnessFraction, 0.01f, 1.0f));
    descs.push_back(floatOpt("ptnrdplanedistancesensitivity", OptionApply::Live,
                             "REBLUR plane distance sensitivity",
                             &GraphicsOptions::ptNrdPlaneDistanceSensitivity, 0.0f, 16.0f));
    descs.push_back(floatOpt("ptnrddisocclusionthreshold", OptionApply::Live,
                             "REBLUR disocclusion threshold",
                             &GraphicsOptions::ptNrdDisocclusionThreshold, 0.0f, 16.0f));
    descs.push_back(boolOpt("ptnrdantifirefly", OptionApply::Live, "enable REBLUR anti-firefly",
                            &GraphicsOptions::ptNrdAntiFirefly));

    // Inert unless the slot runs FSR, but read per dispatch either way.
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

const GraphicsOptionDesc *findGraphicsOptionDesc(const std::string &name) {
    static const std::unordered_map<std::string, const GraphicsOptionDesc *> byName = []() {
        std::unordered_map<std::string, const GraphicsOptionDesc *> result;
        for (const auto &desc : graphicsOptionDescs())
            result.emplace(desc.name, &desc);
        return result;
    }();
    const auto found = byName.find(name);
    return found != byName.end() ? found->second : nullptr;
}

std::vector<std::string> graphicsOptionsDiffering(const GraphicsOptions &left,
                                                  const GraphicsOptions &right,
                                                  OptionApply apply) {
    std::vector<std::string> names;
    for (const auto &desc : graphicsOptionDescs()) {
        if (desc.apply == apply && !desc.equal(left, right))
            names.push_back(desc.name);
    }
    return names;
}

void copyGraphicsOptions(const GraphicsOptions &from, GraphicsOptions &to,
                         OptionApply apply) {
    for (const auto &desc : graphicsOptionDescs()) {
        if (desc.apply == apply)
            desc.copy(from, to);
    }
}

} // namespace graphics

} // namespace reone
