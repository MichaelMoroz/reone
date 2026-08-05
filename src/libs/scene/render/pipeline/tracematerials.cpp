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
#include "reone/scene/render/pipeline/tracematerials.h"

#include <fstream>
#include <sstream>

namespace reone::scene {

bool CuratedMaterial::isDefault() const {
    return klass == TraceClass::Default && albedoMul == glm::vec3(1.0f) &&
           roughnessMode == 0 && metallicMode == 0 && emissionMode == 0;
}

int TraceMaterialOverrides::curatedIndex(const std::string &model, const std::string &node) const {
    auto exact = _curatedIndexByKey.find(model + "/" + node);
    if (exact != _curatedIndexByKey.end()) return exact->second;
    auto wildcard = _curatedIndexByKey.find(model + "/*");
    return wildcard != _curatedIndexByKey.end() ? wildcard->second : -1;
}

const CuratedMaterial *TraceMaterialOverrides::curatedByIndex(int index) const {
    return index >= 0 && index < static_cast<int>(_curatedMaterials.size())
               ? &_curatedMaterials[index]
               : nullptr;
}

CuratedMaterial TraceMaterialOverrides::curatedFor(const std::string &model,
                                                    const std::string &node) const {
    auto index = curatedIndex(model, node);
    return index >= 0 ? _curatedMaterials[index] : CuratedMaterial {};
}

void TraceMaterialOverrides::setCurated(const std::string &model, const std::string &node,
                                        CuratedMaterial curated) {
    auto key = model + "/" + node;
    auto found = _curatedIndexByKey.find(key);
    if (curated.isDefault()) {
        if (found != _curatedIndexByKey.end()) _curatedIndexByKey.erase(found);
    } else if (found != _curatedIndexByKey.end()) {
        _curatedMaterials[found->second] = std::move(curated);
    } else {
        _curatedIndexByKey[key] = static_cast<int>(_curatedMaterials.size());
        _curatedMaterials.push_back(std::move(curated));
    }
    saveTraceClasses();
}

namespace {
const char *traceClassName(TraceClass klass) {
    switch (klass) {
    case TraceClass::Prelit: return "prelit";
    case TraceClass::Emissive: return "emissive";
    case TraceClass::None: return "none";
    default: return "default";
    }
}

TraceClass traceClassFromName(const std::string &name) {
    if (name == "prelit") return TraceClass::Prelit;
    if (name == "emissive") return TraceClass::Emissive;
    if (name == "none") return TraceClass::None;
    return TraceClass::Default;
}

std::vector<float> parseFloats(const std::string &text) {
    std::vector<float> values;
    std::istringstream in(text);
    float value;
    while (in >> value) values.push_back(value);
    return values;
}

void parseChannel(const std::string &value, int &mode, glm::vec4 &params, glm::vec3 &weights) {
    if (value.rfind("curve", 0) == 0) {
        auto numbers = parseFloats(value.substr(5));
        if (numbers.size() >= 4) {
            mode = 2;
            params = {numbers[0], numbers[1], numbers[2], numbers[3]};
            if (numbers.size() >= 7) weights = {numbers[4], numbers[5], numbers[6]};
        }
    } else {
        auto numbers = parseFloats(value);
        if (numbers.size() == 1) {
            mode = 1;
            params.x = numbers[0];
        }
    }
}

std::string formatChannel(int mode, const glm::vec4 &params, const glm::vec3 &weights) {
    std::ostringstream out;
    if (mode == 1) {
        out << params.x;
    } else {
        out << "curve " << params.x << " " << params.y << " " << params.z << " " << params.w;
        if (weights != glm::vec3(0.299f, 0.587f, 0.114f))
            out << " " << weights.x << " " << weights.y << " " << weights.z;
    }
    return out.str();
}
} // namespace

void TraceMaterialOverrides::loadTraceClasses(const std::filesystem::path &path) {
    _traceClassesPath = path;
    _curatedIndexByKey.clear();
    _curatedMaterials.clear();
    std::ifstream in(path);
    if (!in) return;
    auto trim = [](std::string &text) {
        auto begin = text.find_first_not_of(" \t\r");
        auto end = text.find_last_not_of(" \t\r");
        text = begin == std::string::npos ? "" : text.substr(begin, end - begin + 1);
    };
    std::string line;
    std::string section;
    CuratedMaterial current;
    auto flush = [&]() {
        if (!section.empty() && !current.isDefault()) {
            _curatedIndexByKey[section] = static_cast<int>(_curatedMaterials.size());
            _curatedMaterials.push_back(current);
        }
        current = CuratedMaterial {};
    };
    while (std::getline(in, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            flush();
            section = line.substr(1, line.size() - 2);
            trim(section);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        bool multiply = eq > 0 && line[eq - 1] == '*';
        auto key = line.substr(0, multiply ? eq - 1 : eq);
        auto value = line.substr(eq + 1);
        trim(key);
        trim(value);
        if (section.empty()) {
            auto klass = traceClassFromName(value);
            if (klass != TraceClass::Default && !key.empty()) {
                CuratedMaterial legacy;
                legacy.klass = klass;
                _curatedIndexByKey[key] = static_cast<int>(_curatedMaterials.size());
                _curatedMaterials.push_back(legacy);
            }
        } else if (key == "class") {
            current.klass = traceClassFromName(value);
        } else if (key == "albedo" && multiply) {
            auto numbers = parseFloats(value);
            if (numbers.size() >= 3) current.albedoMul = {numbers[0], numbers[1], numbers[2]};
        } else if (key == "roughness") {
            parseChannel(value, current.roughnessMode, current.roughnessParams,
                         current.roughnessWeights);
        } else if (key == "metallic") {
            parseChannel(value, current.metallicMode, current.metallicParams,
                         current.metallicWeights);
        } else if (key == "emission") {
            auto numbers = parseFloats(value);
            if (numbers.size() >= 3) {
                current.emissionMode = multiply ? 1 : 2;
                current.emissionValue = {numbers[0], numbers[1], numbers[2]};
            } else if (numbers.size() == 1 && multiply) {
                current.emissionMode = 1;
                current.emissionValue = glm::vec3(numbers[0]);
            }
        }
    }
    flush();
}

void TraceMaterialOverrides::saveTraceClasses() const {
    if (_traceClassesPath.empty()) return;
    std::ofstream out(_traceClassesPath);
    for (const auto &[key, index] : _curatedIndexByKey) {
        const auto &curated = _curatedMaterials[index];
        out << "[" << key << "]\n";
        if (curated.klass != TraceClass::Default)
            out << "class = " << traceClassName(curated.klass) << "\n";
        if (curated.albedoMul != glm::vec3(1.0f))
            out << "albedo *= " << curated.albedoMul.x << " " << curated.albedoMul.y << " "
                << curated.albedoMul.z << "\n";
        if (curated.roughnessMode != 0)
            out << "roughness = "
                << formatChannel(curated.roughnessMode, curated.roughnessParams,
                                 curated.roughnessWeights)
                << "\n";
        if (curated.metallicMode != 0)
            out << "metallic = "
                << formatChannel(curated.metallicMode, curated.metallicParams,
                                 curated.metallicWeights)
                << "\n";
        if (curated.emissionMode == 1)
            out << "emission *= " << curated.emissionValue.x << " " << curated.emissionValue.y
                << " " << curated.emissionValue.z << "\n";
        else if (curated.emissionMode == 2)
            out << "emission = " << curated.emissionValue.x << " " << curated.emissionValue.y
                << " " << curated.emissionValue.z << "\n";
        out << "\n";
    }
}

} // namespace reone::scene
