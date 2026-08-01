/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace reone::scene {

/** Persisted trace classifications. Values are part of the on-disk contract. */
enum class TraceClass : int {
    Default = 0,
    Prelit = 1,
    Emissive = 2,
    None = 3,
};
static_assert(static_cast<int>(TraceClass::Default) == 0);
static_assert(static_cast<int>(TraceClass::Prelit) == 1);
static_assert(static_cast<int>(TraceClass::Emissive) == 2);
static_assert(static_cast<int>(TraceClass::None) == 3);

struct CuratedMaterial {
    TraceClass klass {TraceClass::Default};
    glm::vec3 albedoMul {1.0f};
    int roughnessMode {0};
    glm::vec4 roughnessParams {0.5f, 0.2f, 0.8f, 1.0f};
    glm::vec3 roughnessWeights {0.299f, 0.587f, 0.114f};
    int metallicMode {0};
    glm::vec4 metallicParams {0.0f, 0.2f, 0.8f, 1.0f};
    glm::vec3 metallicWeights {0.299f, 0.587f, 0.114f};
    int emissionMode {0};
    glm::vec3 emissionValue {1.0f};

    bool isDefault() const;
};

/** Name-keyed trace-only material overrides, independent of a frame snapshot. */
class TraceMaterialOverrides {
public:
    int curatedIndex(const std::string &model, const std::string &node) const;
    const CuratedMaterial *curatedByIndex(int index) const;
    const std::vector<CuratedMaterial> &curatedMaterials() const { return _curatedMaterials; }
    CuratedMaterial curatedFor(const std::string &model, const std::string &node) const;
    void setCurated(const std::string &model, const std::string &node, CuratedMaterial curated);
    void loadTraceClasses(const std::filesystem::path &path);

private:
    std::map<std::string, int> _curatedIndexByKey;
    std::vector<CuratedMaterial> _curatedMaterials;
    std::filesystem::path _traceClassesPath;

    void saveTraceClasses() const;
};

} // namespace reone::scene
