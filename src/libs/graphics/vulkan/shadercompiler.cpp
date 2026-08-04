/*
 * Copyright (c) 2026 The reone project contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "reone/graphics/vulkan/shadercompiler.h"

#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

#include "reone/graphics/gpuscene.h"
#include "reone/system/logutil.h"

namespace reone::graphics {
namespace {

constexpr const char *kModules[] = {
    "pbr_model", "megadraw", "shadow_megadraw", "sky", "grass", "walkmesh", "common",
    "shadow", "pbr_ibl", "particles", "pbr_resolve", "retro_resolve", "pbr_ssao",
    "pbr_ssr", "rayquery", "skin", "nrd_composite", "pt_tonemap", "postprocess", "vk2d"};

uint64_t hashBytes(uint64_t hash, const void *data, size_t size) {
    auto bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hexHash(uint64_t hash) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

struct Field {
    const char *name;
    size_t offset;
};

} // namespace

struct SlangShaderCompiler::Impl {
    Slang::ComPtr<slang::IGlobalSession> global;

    struct Program {
        Slang::ComPtr<slang::ISession> session;
        Slang::ComPtr<slang::IComponentType> linked;
    };

    static std::string diagnostics(slang::IBlob *blob) {
        return blob ? std::string(static_cast<const char *>(blob->getBufferPointer()), blob->getBufferSize()) : "";
    }

    Program load(const std::filesystem::path &sourceDir, const std::string &name) const {
        slang::TargetDesc target {};
        target.format = SLANG_SPIRV;
        target.profile = global->findProfile("spirv_1_5");
        slang::CompilerOptionEntry option {};
        option.name = slang::CompilerOptionName::EmitSpirvDirectly;
        option.value.kind = slang::CompilerOptionValueKind::Int;
        option.value.intValue0 = 1;
        const auto searchPath = sourceDir.string();
        const char *searchPaths[] = {searchPath.c_str()};
        slang::SessionDesc desc {};
        desc.targets = &target;
        desc.targetCount = 1;
        desc.searchPaths = searchPaths;
        desc.searchPathCount = 1;
        desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
        desc.compilerOptionEntries = &option;
        desc.compilerOptionEntryCount = 1;

        Program program;
        Slang::ComPtr<slang::IBlob> diagnostic;
        if (SLANG_FAILED(global->createSession(desc, program.session.writeRef())))
            throw std::runtime_error("Slang: cannot create compilation session for '" + name + "'");
        Slang::ComPtr<slang::IModule> module;
        module.attach(program.session->loadModule(name.c_str(), diagnostic.writeRef()));
        if (!module)
            throw std::runtime_error("Slang: cannot load '" + name + "'\n" + diagnostics(diagnostic));
        diagnostic.setNull();
        if (SLANG_FAILED(module->link(program.linked.writeRef(), diagnostic.writeRef())))
            throw std::runtime_error("Slang: cannot link '" + name + "'\n" + diagnostics(diagnostic));
        return program;
    }
};

SlangShaderCompiler::SlangShaderCompiler(std::filesystem::path sourceDir) :
    _impl(std::make_unique<Impl>()),
    _sourceDir(std::move(sourceDir)) {
    _cacheDir = std::filesystem::temp_directory_path() / "reone" / "slang-cache";
}

SlangShaderCompiler::~SlangShaderCompiler() = default;

void SlangShaderCompiler::init() {
    if (_impl->global)
        return;
    if (SLANG_FAILED(slang::createGlobalSession(_impl->global.writeRef())))
        throw std::runtime_error("Slang: cannot create global session");
    std::filesystem::create_directories(_cacheDir);
    _sourceHash = sourceHash();
    _cacheHits = 0;
    _compiledModules = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const auto *name : kModules)
        module(name);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    info("Slang: loaded " + std::to_string(std::size(kModules)) + " modules (" +
             std::to_string(_compiledModules) + " compiled, " + std::to_string(_cacheHits) +
             " warm-cache) in " + std::to_string(elapsed.count()) + " ms",
         LogChannel::Graphics);
}

void SlangShaderCompiler::deinit() {
    _modules.clear();
    _moduleHashes.clear();
    _impl->global.setNull();
    _sourceHash = 0;
}

uint64_t SlangShaderCompiler::sourceHash() const {
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(_sourceDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".slang")
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    uint64_t hash = 1469598103934665603ull;
    // Cache entries made by the former slangc subprocess are not interchangeable
    // with output produced by this in-process compilation configuration.
    static constexpr char version[] = "reone-slang-spirv-1.5-column-major-inprocess-v2";
    hash = hashBytes(hash, version, sizeof(version) - 1);
    std::array<char, 4096> buffer {};
    for (const auto &file : files) {
        const auto relative = std::filesystem::relative(file, _sourceDir).generic_string();
        hash = hashBytes(hash, relative.data(), relative.size());
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
            throw std::runtime_error("Slang: cannot read shader source " + file.string());
        while (stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || stream.gcount())
            hash = hashBytes(hash, buffer.data(), static_cast<size_t>(stream.gcount()));
    }
    return hash;
}

std::vector<uint32_t> SlangShaderCompiler::compile(const std::string &name, bool &fromCache, bool force) {
    fromCache = false;
    const auto path = _cacheDir / (name + "-" + hexHash(_sourceHash) + ".spv");
    if (!force) {
        std::ifstream cached(path, std::ios::binary | std::ios::ate);
        if (cached.good()) {
            const auto bytes = static_cast<size_t>(cached.tellg());
            if (bytes != 0 && bytes % sizeof(uint32_t) == 0) {
                std::vector<uint32_t> words(bytes / sizeof(uint32_t));
                cached.seekg(0);
                cached.read(reinterpret_cast<char *>(words.data()), static_cast<std::streamsize>(bytes));
                if (cached) {
                    ++_cacheHits;
                    return fromCache = true, words;
                }
            }
        }
    }

    const auto program = _impl->load(_sourceDir, name);
    ++_compiledModules;
    Slang::ComPtr<slang::IBlob> diagnostic;
    Slang::ComPtr<slang::IBlob> code;
    if (SLANG_FAILED(program.linked->getTargetCode(0, code.writeRef(), diagnostic.writeRef())))
        throw std::runtime_error("Slang: cannot compile '" + name + "'\n" + Impl::diagnostics(diagnostic));
    const auto bytes = code->getBufferSize();
    if (bytes == 0 || bytes % sizeof(uint32_t) != 0)
        throw std::runtime_error("Slang: '" + name + "' emitted malformed SPIR-V");
    std::vector<uint32_t> words(bytes / sizeof(uint32_t));
    std::memcpy(words.data(), code->getBufferPointer(), bytes);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Slang: cannot store cache entry for '" + name + "'");
    output.write(reinterpret_cast<const char *>(words.data()), static_cast<std::streamsize>(bytes));
    if (!output)
        throw std::runtime_error("Slang: cannot store cache entry for '" + name + "'");
    return words;
}

const std::vector<uint32_t> &SlangShaderCompiler::module(const std::string &name) {
    const auto hash = sourceHash();
    _sourceHash = hash;
    auto existing = _modules.find(name);
    if (existing != _modules.end() && _moduleHashes[name] == hash)
        return existing->second;
    try {
        bool fromCache = false;
        auto words = compile(name, fromCache);
        debug("Slang: " + name + (fromCache ? " loaded from cache" : " compiled"),
              LogChannel::Graphics);
        _modules.insert_or_assign(name, std::move(words));
        _moduleHashes[name] = hash;
        return _modules.at(name);
    } catch (const std::exception &e) {
        error(e.what(), LogChannel::Graphics);
        if (existing != _modules.end())
            return existing->second;
        throw;
    }
}

bool SlangShaderCompiler::recompileAll() {
    const auto previous = _modules;
    _modules.clear();
    _moduleHashes.clear();
    _sourceHash = sourceHash();
    bool success = true;
    for (const auto *name : kModules) {
        try {
            bool ignored = false;
            _modules.emplace(name, compile(name, ignored, true));
            _moduleHashes.emplace(name, _sourceHash);
        } catch (const std::exception &e) {
            error(e.what(), LogChannel::Graphics);
            auto old = previous.find(name);
            if (old != previous.end())
                _modules.emplace(name, old->second);
            success = false;
        }
    }
    return success;
}

void SlangShaderCompiler::invalidate() {
    _modules.clear();
    _moduleHashes.clear();
    _sourceHash = sourceHash();
}

void SlangShaderCompiler::validateSceneSchema() {
    const auto program = _impl->load(_sourceDir, "scene_schema_reflect");
    Slang::ComPtr<slang::IBlob> diagnostic;
    auto *layout = program.linked->getLayout(0, diagnostic.writeRef());
    if (!layout)
        throw std::runtime_error("Slang: cannot reflect scene schema\n" + Impl::diagnostics(diagnostic));
    const auto check = [layout](const char *slangName, const char *cppName, size_t cppSize,
                                std::initializer_list<Field> expectedFields) {
        auto *type = layout->findTypeByName(slangName);
        auto *typeLayout = type ? layout->getTypeLayout(type, slang::LayoutRules::Default) : nullptr;
        if (!typeLayout)
            throw std::runtime_error("Slang schema reflection omitted " + std::string(slangName));
        for (const auto &expected : expectedFields) {
            auto *field = typeLayout->getFieldByIndex(0);
            for (unsigned int i = 0; i < typeLayout->getFieldCount(); ++i) {
                auto *candidate = typeLayout->getFieldByIndex(i);
                if (std::strcmp(candidate->getName(), expected.name) == 0) {
                    field = candidate;
                    break;
                }
                field = nullptr;
            }
            if (!field)
                throw std::runtime_error("Slang schema mismatch: " + std::string(cppName) + " is missing field " + expected.name);
            const auto offset = field->getOffset(slang::ParameterCategory::Uniform);
            if (offset != expected.offset)
                throw std::runtime_error("Slang schema mismatch: " + std::string(cppName) + "::" + expected.name + " is at " + std::to_string(expected.offset) + ", Slang expects " + std::to_string(offset));
        }
        const auto stride = typeLayout->getStride(slang::ParameterCategory::Uniform);
        if (stride != cppSize)
            throw std::runtime_error("Slang schema mismatch: " + std::string(cppName) + " has stride " + std::to_string(cppSize) + ", Slang expects " + std::to_string(stride));
    };
    check("InstanceMaterial", "InstanceMaterial", sizeof(InstanceMaterial),
          {{"mainTex", offsetof(InstanceMaterial, mainTex)}, {"envMap", offsetof(InstanceMaterial, envMap)}, {"curatedEmission", offsetof(InstanceMaterial, curatedEmission)}, {"surfaceType", offsetof(InstanceMaterial, surfaceType)}, {"envMapDerivedLayer", offsetof(InstanceMaterial, envMapDerivedLayer)}});
    check("Matrix3x4", "GpuSceneMatrix3x4", sizeof(GpuSceneMatrix3x4), {{"row0", offsetof(GpuSceneMatrix3x4, row0)}, {"row2", offsetof(GpuSceneMatrix3x4, row2)}});
    check("MergedVertex", "MergedVertex", sizeof(MergedVertex), {{"position", offsetof(MergedVertex, position)}, {"normal", offsetof(MergedVertex, normal)}, {"uv1", offsetof(MergedVertex, uv1)}, {"prevPosition", offsetof(MergedVertex, prevPosition)}, {"color", offsetof(MergedVertex, color)}});
    check("SceneObject", "SceneObject", sizeof(SceneObject), {{"srcVertexOffset", offsetof(SceneObject, srcVertexOffset)}, {"vertexCount", offsetof(SceneObject, vertexCount)}, {"materialIndex", offsetof(SceneObject, materialIndex)}, {"saberDisplacement", offsetof(SceneObject, saberDisplacement)}});
    check("ProceduralQuad", "GpuSceneProceduralQuad", sizeof(GpuSceneProceduralQuad), {{"positionVariant", offsetof(GpuSceneProceduralQuad, positionVariant)}, {"color", offsetof(GpuSceneProceduralQuad, color)}});
    check("GrassFace", "GpuSceneGrassFace", sizeof(GpuSceneGrassFace), {{"vertex0Uv0x", offsetof(GpuSceneGrassFace, vertex0Uv0x)}, {"faceBudgetMaterialVariants", offsetof(GpuSceneGrassFace, faceBudgetMaterialVariants)}});
    check("GrassRange", "GpuSceneGrassRange", sizeof(GpuSceneGrassRange), {{"faceIndex", offsetof(GpuSceneGrassRange, faceIndex)}, {"pad", offsetof(GpuSceneGrassRange, pad)}});
}

} // namespace reone::graphics
