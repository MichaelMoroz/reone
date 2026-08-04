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

/**
 * One global session for the process, created on first use and deliberately
 * never released.
 *
 * The session owns Slang's compiler back-end DLLs. Releasing it - which every
 * SlangShaderCompiler::deinit used to do - is the one act both crashing
 * binaries had in common, and Slang's own guidance is that a program creates
 * exactly one and keeps it, because creation is expensive. Leaking it at exit
 * costs nothing the operating system does not reclaim.
 */
slang::IGlobalSession *globalSession() {
    static slang::IGlobalSession *session = [] {
        Slang::ComPtr<slang::IGlobalSession> created;
        if (SLANG_FAILED(slang::createGlobalSession(created.writeRef())))
            throw std::runtime_error("Slang: cannot create global session");
        return created.detach();
    }();
    return session;
}

} // namespace

struct SlangShaderCompiler::Impl {
    /** Borrowed from globalSession(); this type never owns it. */
    slang::IGlobalSession *global {nullptr};

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
    _impl->global = globalSession();
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
    _impl->global = nullptr;
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
    // Deliberately does not re-read the source tree. Hashing 41 files to answer
    // "is this module current" would run on every lazy pipeline creation, which
    // happens mid-game. init() and the explicit reload paths own the hash.
    const auto hash = _sourceHash;
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
    // Every Slang field must have a C++ member at the same offset. Checking the
    // Slang side exhaustively, rather than a sample, is what catches a swap of
    // two adjacent fields of the same size - a stride comparison alone cannot.
    // The C++ mirrors carry explicit padding members that Slang derives from
    // its own alignment rules, so extra C++ members are expected and ignored.
    const auto check = [layout](const char *name, size_t cppSize,
                                std::initializer_list<Field> mirror) {
        auto *type = layout->findTypeByName(name);
        auto *typeLayout = type ? layout->getTypeLayout(type, slang::LayoutRules::Default) : nullptr;
        if (!typeLayout)
            throw std::runtime_error("Slang schema reflection omitted " + std::string(name));
        const auto mismatch = [name](const std::string &detail) {
            return std::runtime_error("Slang schema mismatch: " + std::string(name) + detail);
        };
        std::string undeclared;
        unsigned int checked = 0;
        for (unsigned int i = 0; i < typeLayout->getFieldCount(); ++i) {
            // Reflection hands back a null entry for a field it did not lay
            // out; dereferencing it is an access violation rather than a
            // diagnostic, so anything unnamed is counted out below instead.
            auto *field = typeLayout->getFieldByIndex(i);
            const auto *fieldName = field ? field->getName() : nullptr;
            if (!fieldName)
                continue;
            ++checked;
            const Field *expected = nullptr;
            for (const auto &candidate : mirror) {
                if (std::strcmp(candidate.name, fieldName) == 0) {
                    expected = &candidate;
                    break;
                }
            }
            // A field the mirror never heard of is reported only after every
            // shared field has been offset-checked, because an inserted field
            // shifts its neighbours and those offsets name the fault better.
            if (!expected) {
                if (!undeclared.empty())
                    undeclared += ", ";
                undeclared += fieldName;
                continue;
            }
            const auto offset = field->getOffset(slang::ParameterCategory::Uniform);
            if (offset != expected->offset)
                throw mismatch("::" + std::string(fieldName) + " is at " + std::to_string(expected->offset) +
                               " in C++, " + std::to_string(offset) + " in Slang");
        }
        if (!undeclared.empty())
            throw mismatch(" declares " + undeclared + ", which the C++ mirror does not");
        // Guards the guard: a mirror entry with no reflected counterpart would
        // otherwise make this check quietly weaker rather than fail.
        if (checked != mirror.size())
            throw mismatch(" reflected " + std::to_string(checked) + " named fields against " +
                           std::to_string(mirror.size()) + " in the C++ mirror");
        const auto stride = typeLayout->getStride(slang::ParameterCategory::Uniform);
        if (stride != cppSize)
            throw mismatch(" has stride " + std::to_string(cppSize) + " in C++, " + std::to_string(stride) + " in Slang");
    };
#define REONE_SCHEMA_FIELD(type, field) \
    Field { #field, offsetof(type, field) }
    check("InstanceMaterial", sizeof(InstanceMaterial),
          {REONE_SCHEMA_FIELD(InstanceMaterial, selfIllumColor),
           REONE_SCHEMA_FIELD(InstanceMaterial, diffuseColor),
           REONE_SCHEMA_FIELD(InstanceMaterial, uv0),
           REONE_SCHEMA_FIELD(InstanceMaterial, uv1),
           REONE_SCHEMA_FIELD(InstanceMaterial, uv2),
           REONE_SCHEMA_FIELD(InstanceMaterial, mainTex),
           REONE_SCHEMA_FIELD(InstanceMaterial, normalMap),
           REONE_SCHEMA_FIELD(InstanceMaterial, lightmap),
           REONE_SCHEMA_FIELD(InstanceMaterial, bumpMapArray),
           REONE_SCHEMA_FIELD(InstanceMaterial, featureMask),
           REONE_SCHEMA_FIELD(InstanceMaterial, bumpMapFrame),
           REONE_SCHEMA_FIELD(InstanceMaterial, bumpMapScale),
           REONE_SCHEMA_FIELD(InstanceMaterial, envMap),
           REONE_SCHEMA_FIELD(InstanceMaterial, curatedAlbedoMul),
           REONE_SCHEMA_FIELD(InstanceMaterial, curatedRoughA),
           REONE_SCHEMA_FIELD(InstanceMaterial, curatedRoughB),
           REONE_SCHEMA_FIELD(InstanceMaterial, curatedMetalA),
           REONE_SCHEMA_FIELD(InstanceMaterial, curatedMetalB),
           REONE_SCHEMA_FIELD(InstanceMaterial, curatedEmission),
           REONE_SCHEMA_FIELD(InstanceMaterial, surfaceType),
           REONE_SCHEMA_FIELD(InstanceMaterial, roughnessScale),
           REONE_SCHEMA_FIELD(InstanceMaterial, envMapCube),
           REONE_SCHEMA_FIELD(InstanceMaterial, waterAlpha),
           REONE_SCHEMA_FIELD(InstanceMaterial, overrideColor),
           REONE_SCHEMA_FIELD(InstanceMaterial, overrideParams),
           REONE_SCHEMA_FIELD(InstanceMaterial, ambientColor),
           REONE_SCHEMA_FIELD(InstanceMaterial, envMapDerivedLayer)});
    check("Matrix3x4", sizeof(Matrix3x4),
          {REONE_SCHEMA_FIELD(Matrix3x4, row0),
           REONE_SCHEMA_FIELD(Matrix3x4, row1),
           REONE_SCHEMA_FIELD(Matrix3x4, row2)});
    check("MergedVertex", sizeof(MergedVertex),
          {REONE_SCHEMA_FIELD(MergedVertex, position),
           REONE_SCHEMA_FIELD(MergedVertex, normal),
           REONE_SCHEMA_FIELD(MergedVertex, uv1),
           REONE_SCHEMA_FIELD(MergedVertex, uv2),
           REONE_SCHEMA_FIELD(MergedVertex, tangent),
           REONE_SCHEMA_FIELD(MergedVertex, bitangent),
           REONE_SCHEMA_FIELD(MergedVertex, tanSpaceNormal),
           REONE_SCHEMA_FIELD(MergedVertex, prevPosition),
           REONE_SCHEMA_FIELD(MergedVertex, pad),
           REONE_SCHEMA_FIELD(MergedVertex, color)});
    check("SceneObject", sizeof(SceneObject),
          {REONE_SCHEMA_FIELD(SceneObject, transform),
           REONE_SCHEMA_FIELD(SceneObject, prevTransform),
           REONE_SCHEMA_FIELD(SceneObject, transformInv),
           REONE_SCHEMA_FIELD(SceneObject, srcVertexOffset),
           REONE_SCHEMA_FIELD(SceneObject, srcIndexOffset),
           REONE_SCHEMA_FIELD(SceneObject, srcVertexStride),
           REONE_SCHEMA_FIELD(SceneObject, offPosition),
           REONE_SCHEMA_FIELD(SceneObject, offNormals),
           REONE_SCHEMA_FIELD(SceneObject, offUV1),
           REONE_SCHEMA_FIELD(SceneObject, offUV2),
           REONE_SCHEMA_FIELD(SceneObject, offTanSpace),
           REONE_SCHEMA_FIELD(SceneObject, offBoneIndices),
           REONE_SCHEMA_FIELD(SceneObject, offBoneWeights),
           REONE_SCHEMA_FIELD(SceneObject, vertexCount),
           REONE_SCHEMA_FIELD(SceneObject, triangleCount),
           REONE_SCHEMA_FIELD(SceneObject, dstVertexBase),
           REONE_SCHEMA_FIELD(SceneObject, dstTriangleBase),
           REONE_SCHEMA_FIELD(SceneObject, geometryIndex),
           REONE_SCHEMA_FIELD(SceneObject, boneBase),
           REONE_SCHEMA_FIELD(SceneObject, boneCount),
           REONE_SCHEMA_FIELD(SceneObject, materialIndex),
           REONE_SCHEMA_FIELD(SceneObject, danglyBase),
           REONE_SCHEMA_FIELD(SceneObject, danglyCount),
           REONE_SCHEMA_FIELD(SceneObject, saberDisplacement)});
    check("ProceduralQuad", sizeof(ProceduralQuad),
          {REONE_SCHEMA_FIELD(ProceduralQuad, positionVariant),
           REONE_SCHEMA_FIELD(ProceduralQuad, right),
           REONE_SCHEMA_FIELD(ProceduralQuad, up),
           REONE_SCHEMA_FIELD(ProceduralQuad, uvOffsetScale),
           REONE_SCHEMA_FIELD(ProceduralQuad, lightmapUV),
           REONE_SCHEMA_FIELD(ProceduralQuad, pad),
           REONE_SCHEMA_FIELD(ProceduralQuad, color)});
    check("GrassFace", sizeof(GrassFace),
          {REONE_SCHEMA_FIELD(GrassFace, vertex0Uv0x),
           REONE_SCHEMA_FIELD(GrassFace, vertex1Uv0y),
           REONE_SCHEMA_FIELD(GrassFace, vertex2Uv1x),
           REONE_SCHEMA_FIELD(GrassFace, uv1yUv2QuadSize),
           REONE_SCHEMA_FIELD(GrassFace, probabilities),
           REONE_SCHEMA_FIELD(GrassFace, boundsMin),
           REONE_SCHEMA_FIELD(GrassFace, boundsMax),
           REONE_SCHEMA_FIELD(GrassFace, faceBudgetMaterialVariants)});
    check("GrassRange", sizeof(GrassRange),
          {REONE_SCHEMA_FIELD(GrassRange, faceIndex),
           REONE_SCHEMA_FIELD(GrassRange, clusterOffset),
           REONE_SCHEMA_FIELD(GrassRange, clusterCount),
           REONE_SCHEMA_FIELD(GrassRange, pad)});
#undef REONE_SCHEMA_FIELD
}

} // namespace reone::graphics
