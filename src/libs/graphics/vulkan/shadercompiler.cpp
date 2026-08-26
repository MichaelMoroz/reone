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
#include "reone/graphics/vulkan/shadercompiler.h"

#include <optional>

#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <unordered_set>

#include "reone/graphics/rendering/gpuscene.h"
#include "reone/graphics/uniforms.h"
#include "reone/system/logutil.h"

namespace reone::graphics {
namespace {

constexpr const char *kModules[] = {
    "scene_draw", "sky", "pbr_ibl", "pbr_resolve", "retro_resolve",
    "path_trace", "nrd_resolve", "shadow_filter", "scene_resolve", "postprocess", "vk2d",
    "debug_view"};

const char *parameterCategoryName(slang::ParameterCategory category) {
    switch (category) {
    case slang::None: return "none";
    case slang::Mixed: return "mixed";
    case slang::ConstantBuffer: return "constant buffer";
    case slang::ShaderResource: return "shader resource";
    case slang::UnorderedAccess: return "unordered access";
    case slang::VaryingInput: return "varying input";
    case slang::VaryingOutput: return "varying output";
    case slang::SamplerState: return "sampler state";
    case slang::Uniform: return "uniform";
    case slang::DescriptorTableSlot: return "descriptor table slot";
    case slang::SpecializationConstant: return "specialization constant";
    case slang::PushConstantBuffer: return "push constant buffer";
    case slang::RegisterSpace: return "register space";
    case slang::GenericResource: return "generic resource";
    case slang::RayPayload: return "ray payload";
    case slang::HitAttributes: return "hit attributes";
    case slang::CallablePayload: return "callable payload";
    case slang::ShaderRecord: return "shader record";
    case slang::ExistentialTypeParam: return "existential type parameter";
    case slang::ExistentialObjectParam: return "existential object parameter";
    case slang::SubElementRegisterSpace: return "sub-element register space";
    case slang::InputAttachmentIndex: return "input attachment index";
    case slang::MetalArgumentBufferElement: return "metal argument buffer element";
    case slang::MetalAttribute: return "metal attribute";
    case slang::MetalPayload: return "metal payload";
    default: return "unknown";
    }
}

const char *bindingTypeName(slang::BindingType type) {
    switch (type) {
    case slang::BindingType::Unknown: return "unknown";
    case slang::BindingType::Sampler: return "sampler";
    case slang::BindingType::Texture: return "texture";
    case slang::BindingType::ConstantBuffer: return "constant buffer";
    case slang::BindingType::ParameterBlock: return "parameter block";
    case slang::BindingType::TypedBuffer: return "typed buffer";
    case slang::BindingType::RawBuffer: return "raw buffer";
    case slang::BindingType::CombinedTextureSampler: return "combined texture sampler";
    case slang::BindingType::InputRenderTarget: return "input render target";
    case slang::BindingType::InlineUniformData: return "inline uniform data";
    case slang::BindingType::RayTracingAccelerationStructure: return "ray tracing acceleration structure";
    case slang::BindingType::VaryingInput: return "varying input";
    case slang::BindingType::VaryingOutput: return "varying output";
    case slang::BindingType::ExistentialValue: return "existential value";
    case slang::BindingType::PushConstant: return "push constant";
    default: return "unknown";
    }
}

const char *typeKindName(slang::TypeReflection::Kind kind) {
    switch (kind) {
    case slang::TypeReflection::Kind::None: return "none";
    case slang::TypeReflection::Kind::Struct: return "struct";
    case slang::TypeReflection::Kind::Array: return "array";
    case slang::TypeReflection::Kind::ConstantBuffer: return "constant buffer";
    case slang::TypeReflection::Kind::Resource: return "resource";
    case slang::TypeReflection::Kind::SamplerState: return "sampler state";
    case slang::TypeReflection::Kind::TextureBuffer: return "texture buffer";
    case slang::TypeReflection::Kind::ShaderStorageBuffer: return "shader storage buffer";
    case slang::TypeReflection::Kind::ParameterBlock: return "parameter block";
    case slang::TypeReflection::Kind::DynamicResource: return "dynamic resource";
    default: return "other";
    }
}

const char *resourceShapeName(SlangResourceShape shape) {
    switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK) {
    case SLANG_RESOURCE_NONE: return "none";
    case SLANG_TEXTURE_1D: return "texture 1D";
    case SLANG_TEXTURE_2D: return "texture 2D";
    case SLANG_TEXTURE_3D: return "texture 3D";
    case SLANG_TEXTURE_CUBE: return "texture cube";
    case SLANG_TEXTURE_BUFFER: return "texture buffer";
    case SLANG_STRUCTURED_BUFFER: return "structured buffer";
    case SLANG_BYTE_ADDRESS_BUFFER: return "byte address buffer";
    case SLANG_ACCELERATION_STRUCTURE: return "acceleration structure";
    case SLANG_TEXTURE_SUBPASS: return "subpass input";
    default: return "unknown";
    }
}

const char *resourceAccessName(SlangResourceAccess access) {
    switch (access) {
    case SLANG_RESOURCE_ACCESS_NONE: return "none";
    case SLANG_RESOURCE_ACCESS_READ: return "read";
    case SLANG_RESOURCE_ACCESS_READ_WRITE: return "read-write";
    case SLANG_RESOURCE_ACCESS_RASTER_ORDERED: return "raster-ordered";
    case SLANG_RESOURCE_ACCESS_APPEND: return "append";
    case SLANG_RESOURCE_ACCESS_CONSUME: return "consume";
    case SLANG_RESOURCE_ACCESS_WRITE: return "write";
    case SLANG_RESOURCE_ACCESS_FEEDBACK: return "feedback";
    default: return "unknown";
    }
}

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

    static SlangStage slangStage(ShaderStage stage) {
        switch (stage) {
        case ShaderStage::Vertex: return SLANG_STAGE_VERTEX;
        case ShaderStage::Fragment: return SLANG_STAGE_FRAGMENT;
        case ShaderStage::Compute: return SLANG_STAGE_COMPUTE;
        case ShaderStage::RayGeneration: return SLANG_STAGE_RAY_GENERATION;
        default: throw std::invalid_argument("Slang: entry point has no execution stage");
        }
    }

    Program load(const std::filesystem::path &sourceDir, const std::string &name,
                 const std::vector<ShaderEntryPoint> &entryPoints = {}) const {
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
        // ISession retains loaded modules. acquire a reference for this local
        // owner instead of attaching the session's borrowed cache entry.
        module = program.session->loadModule(name.c_str(), diagnostic.writeRef());
        if (!module)
            throw std::runtime_error("Slang: cannot load '" + name + "'\n" + diagnostics(diagnostic));
        if (entryPoints.empty()) {
            diagnostic.setNull();
            if (SLANG_FAILED(module->link(program.linked.writeRef(), diagnostic.writeRef())))
                throw std::runtime_error("Slang: cannot link '" + name + "'\n" + diagnostics(diagnostic));
        } else {
            std::vector<Slang::ComPtr<slang::IEntryPoint>> selected;
            std::vector<slang::IComponentType *> components {module.get()};
            selected.reserve(entryPoints.size());
            components.reserve(entryPoints.size() + 1);
            for (const auto &entry : entryPoints) {
                Slang::ComPtr<slang::IEntryPoint> selectedEntry;
                diagnostic.setNull();
                if (SLANG_FAILED(module->findAndCheckEntryPoint(
                        entry.name.c_str(), slangStage(entry.stage),
                        selectedEntry.writeRef(), diagnostic.writeRef()))) {
                    throw std::runtime_error("Slang: cannot select entry point '" + entry.name +
                                             "' in '" + name + "'\n" + diagnostics(diagnostic));
                }
                components.push_back(selectedEntry.get());
                selected.push_back(std::move(selectedEntry));
            }
            Slang::ComPtr<slang::IComponentType> composite;
            diagnostic.setNull();
            if (SLANG_FAILED(program.session->createCompositeComponentType(
                    components.data(), static_cast<SlangInt>(components.size()),
                    composite.writeRef(), diagnostic.writeRef()))) {
                throw std::runtime_error("Slang: cannot compose entry points for '" + name +
                                         "'\n" + diagnostics(diagnostic));
            }
            diagnostic.setNull();
            if (SLANG_FAILED(composite->link(program.linked.writeRef(), diagnostic.writeRef())))
                throw std::runtime_error("Slang: cannot link entry points for '" + name +
                                         "'\n" + diagnostics(diagnostic));
        }
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

ShaderReflection SlangShaderCompiler::reflection(const std::string &name) const {
    return reflection(name, {});
}

std::unordered_set<std::string> spirvPushConstantTypes(
    slang::IBlob &blob, const std::string &shader) {
    const auto bytes = blob.getBufferSize();
    if (bytes < 5 * sizeof(uint32_t) || bytes % sizeof(uint32_t) != 0)
        throw std::runtime_error("Slang: malformed entry-point SPIR-V for '" + shader + "'");
    const auto *words = static_cast<const uint32_t *>(blob.getBufferPointer());
    const auto wordCount = bytes / sizeof(uint32_t);
    if (words[0] != 0x07230203u)
        throw std::runtime_error("Slang: malformed entry-point SPIR-V for '" + shader + "'");

    std::unordered_map<uint32_t, std::string> names;
    std::unordered_map<uint32_t, uint32_t> pushPointers;
    std::vector<uint32_t> pushVariables;
    for (size_t offset = 5; offset < wordCount;) {
        const uint32_t count = words[offset] >> 16;
        const uint32_t opcode = words[offset] & 0xffffu;
        if (count == 0 || count > wordCount - offset)
            throw std::runtime_error("Slang: malformed entry-point SPIR-V for '" + shader + "'");
        if (opcode == 5u && count > 2) { // OpName
            std::string name;
            bool terminated = false;
            for (size_t word = offset + 2; word < offset + count && !terminated; ++word) {
                for (int byte = 0; byte < 4; ++byte) {
                    const char c = static_cast<char>((words[word] >> (8 * byte)) & 0xffu);
                    if (c == '\0') {
                        terminated = true;
                        break;
                    }
                    name.push_back(c);
                }
            }
            names.emplace(words[offset + 1], std::move(name));
        } else if (opcode == 32u && count >= 4 && words[offset + 2] == 9u) { // OpTypePointer PushConstant
            pushPointers.emplace(words[offset + 1], words[offset + 3]);
        } else if (opcode == 59u && count >= 4 && words[offset + 3] == 9u) { // OpVariable PushConstant
            pushVariables.push_back(words[offset + 1]);
        }
        offset += count;
    }

    std::unordered_set<std::string> result;
    for (const auto pointer : pushVariables) {
        const auto pointed = pushPointers.find(pointer);
        if (pointed == pushPointers.end())
            throw std::runtime_error("Slang: unnamed push-constant type in '" + shader + "'");
        const auto named = names.find(pointed->second);
        if (named == names.end())
            throw std::runtime_error("Slang: unnamed push-constant type in '" + shader + "'");
        auto name = named->second;
        if (const auto suffix = name.find("_std430"); suffix != std::string::npos)
            name.erase(suffix);
        result.insert(std::move(name));
    }
    return result;
}

ShaderReflection SlangShaderCompiler::reflection(
    const std::string &name, const std::vector<ShaderEntryPoint> &entryPoints) const {
    const auto program = _impl->load(_sourceDir, name, entryPoints);
    Slang::ComPtr<slang::IBlob> diagnostic;
    auto *layout = program.linked->getLayout(0, diagnostic.writeRef());
    if (!layout)
        throw std::runtime_error("Slang: cannot reflect bindings for '" + name + "'\n" +
                                 Impl::diagnostics(diagnostic));
    auto *globalParameters = layout->getGlobalParamsTypeLayout();
    if (!globalParameters)
        throw std::runtime_error("Slang: reflection has no global parameters for '" + name + "'");

    ShaderReflection result;
    // The load path links a module without explicitly adding its defined entry
    // points, so the program layout may legitimately report zero of them even
    // though the source declares one. The stage is therefore advisory: taken
    // from reflection when an entry point is present, left Unknown otherwise,
    // and each pipeline kind applies its own stage as it always did. More than
    // one entry point is still a real error - which kernel to bind would be
    // ambiguous.
    if (layout->getEntryPointCount() > 1 && entryPoints.empty())
        throw std::runtime_error("Slang: more than one entry point in '" + name + "'");
    if (layout->getEntryPointCount() == 1) {
        const auto stage = layout->getEntryPointByIndex(0)->getStage();
        if (stage == SLANG_STAGE_VERTEX)
            result.stage = ShaderStage::Vertex;
        else if (stage == SLANG_STAGE_FRAGMENT)
            result.stage = ShaderStage::Fragment;
        else if (stage == SLANG_STAGE_COMPUTE)
            result.stage = ShaderStage::Compute;
        else if (stage == SLANG_STAGE_RAY_GENERATION)
            result.stage = ShaderStage::RayGeneration;
        else
            throw std::runtime_error("Slang: unsupported reflected entry-point stage in '" + name + "'");
    }
    std::vector<Slang::ComPtr<slang::IMetadata>> metadata;
    std::unordered_set<std::string> usedPushConstantTypes;
    metadata.reserve(entryPoints.size());
    for (size_t index = 0; index < entryPoints.size(); ++index) {
        Slang::ComPtr<slang::IBlob> code;
        diagnostic.setNull();
        if (SLANG_FAILED(program.linked->getEntryPointCode(
                static_cast<SlangInt>(index), 0, code.writeRef(), diagnostic.writeRef()))) {
            throw std::runtime_error("Slang: cannot generate entry-point metadata for '" + name +
                                     "'\n" + Impl::diagnostics(diagnostic));
        }
        if (!code)
            throw std::runtime_error("Slang: entry point generated no SPIR-V for '" + name + "'");
        auto pushTypes = spirvPushConstantTypes(*code, name);
        usedPushConstantTypes.insert(pushTypes.begin(), pushTypes.end());
        Slang::ComPtr<slang::IMetadata> entryMetadata;
        diagnostic.setNull();
        if (SLANG_FAILED(program.linked->getEntryPointMetadata(
                static_cast<SlangInt>(index), 0, entryMetadata.writeRef(),
                diagnostic.writeRef()))) {
            throw std::runtime_error("Slang: cannot reflect entry-point usage for '" + name +
                                     "'\n" + Impl::diagnostics(diagnostic));
        }
        metadata.push_back(std::move(entryMetadata));
    }
    // Walk the program's global parameters through their variable layouts
    // rather than the type layout's binding ranges. The binding-range API
    // would be tidier, but this Slang release ships its space accessors
    // commented out of slang.h, and the descriptor-set *index* it does expose
    // is not the *space* an explicit [[vk::binding(b, s)]] declares - the two
    // agree only for single-set shaders, which is how the ranges walk survived
    // until skin (frame uniforms in space 0, resources in space 1). Variable
    // layouts report the annotated space directly.
    const auto paramCount = layout->getParameterCount();
    for (unsigned int index = 0; index < paramCount; ++index) {
        auto *param = layout->getParameterByIndex(index);
        if (!param)
            continue;
        const auto *paramName = param->getName();
        auto *typeLayout = param->getTypeLayout();
        if (!typeLayout)
            throw std::runtime_error("Slang: parameter without a type layout in '" + name + "'");

        const auto locationUsedBySelectedEntry = [&](slang::ParameterCategory category,
                                                     SlangUInt space, SlangUInt binding) {
            if (metadata.empty())
                return true;
            for (const auto &entryMetadata : metadata) {
                bool used = false;
                if (SLANG_FAILED(entryMetadata->isParameterLocationUsed(
                        static_cast<SlangParameterCategory>(category),
                        space, binding, used))) {
                    throw std::runtime_error("Slang: cannot query parameter usage in '" + name + "'");
                }
                if (used)
                    return true;
            }
            return false;
        };
        const auto usedBySelectedEntry = [&](slang::ParameterCategory category) {
            return locationUsedBySelectedEntry(
                category,
                static_cast<SlangUInt>(param->getBindingSpace(category)),
                static_cast<SlangUInt>(param->getOffset(category)));
        };

        bool isPushConstant = false, isDescriptor = false;
        const auto categoryCount = typeLayout->getCategoryCount();
        for (unsigned int c = 0; c < categoryCount; ++c) {
            const auto category = typeLayout->getCategoryByIndex(c);
            if (category == slang::PushConstantBuffer)
                isPushConstant = true;
            else if (category == slang::DescriptorTableSlot)
                isDescriptor = true;
        }
        if (isPushConstant) {
            // getSize(PushConstantBuffer) counts whole ranges, not bytes; the
            // byte size is the wrapped element's uniform-category size.
            auto *element = typeLayout->getElementTypeLayout();
            const auto size = element ? element->getSize() : typeLayout->getSize();
            if (size == 0 || size > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("Slang: malformed push constants in '" + name + "'");
            if (!entryPoints.empty()) {
                const auto *typeName = element ? element->getName() : nullptr;
                if (!typeName || usedPushConstantTypes.find(typeName) == usedPushConstantTypes.end())
                    continue;
            }
            if (result.pushConstantSize != 0 && entryPoints.empty())
                throw std::runtime_error("Slang: multiple push constant ranges in '" + name + "'");
            result.pushConstantSize = std::max(result.pushConstantSize,
                                               static_cast<uint32_t>(size));
            continue;
        }
        if (!isDescriptor)
            continue; // varyings, specialization constants, plain uniform data
        if (!usedBySelectedEntry(slang::DescriptorTableSlot))
            continue;

        const auto set = param->getBindingSpace(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT);
        const auto binding = param->getOffset(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT);

        // Arrays bind their element type at a count; an unsized array remains
        // explicitly unsized.  Treating it as one made descriptor-indexing
        // shaders look like ordinary scalar bindings to callers, which then
        // had no way to apply their API-specific bindless policy.
        auto *resource = typeLayout;
        size_t count = 1;
        while (resource && resource->getKind() == slang::TypeReflection::Kind::Array) {
            const auto elementCount = resource->getElementCount();
            if (elementCount == 0) {
                count = 0;
                resource = resource->getElementTypeLayout();
                break;
            }
            count *= elementCount;
            resource = resource->getElementTypeLayout();
        }
        if (!paramName)
            throw std::runtime_error("Slang: incomplete binding reflection for '" + name + "'");

        const auto reflectionError = [&] {
            const auto leafKind = resource ? typeKindName(resource->getKind()) : "none";
            const auto shape = resource ? resourceShapeName(resource->getResourceShape()) : "none";
            const auto access = resource ? resourceAccessName(resource->getResourceAccess()) : "none";
            return "Slang: unsupported reflected binding '" + std::string(paramName) + "' in '" +
                   name + "': leaf kind " + std::string(leafKind) + ", resource shape " + shape +
                   ", resource access " + access;
        };

        std::optional<ShaderResourceKind> resolved;
        switch (resource ? resource->getKind() : slang::TypeReflection::Kind::None) {
        case slang::TypeReflection::Kind::ConstantBuffer:
            resolved = ShaderResourceKind::UniformBuffer;
            break;
        case slang::TypeReflection::Kind::SamplerState:
            resolved = ShaderResourceKind::Sampler;
            break;
        case slang::TypeReflection::Kind::Resource:
        case slang::TypeReflection::Kind::TextureBuffer:
        case slang::TypeReflection::Kind::ShaderStorageBuffer: {
            const auto fullShape = resource->getResourceShape();
            const auto baseShape = fullShape & SLANG_RESOURCE_BASE_SHAPE_MASK;
            const auto readOnly = resource->getResourceAccess() == SLANG_RESOURCE_ACCESS_READ;
            const bool combined = (fullShape & SLANG_TEXTURE_COMBINED_FLAG) != 0;
            switch (baseShape) {
            case SLANG_STRUCTURED_BUFFER:
            case SLANG_BYTE_ADDRESS_BUFFER:
                resolved = ShaderResourceKind::StorageBuffer;
                break;
            case SLANG_TEXTURE_1D:
            case SLANG_TEXTURE_2D:
            case SLANG_TEXTURE_3D:
            case SLANG_TEXTURE_CUBE:
            case SLANG_TEXTURE_BUFFER:
                resolved = combined ? ShaderResourceKind::CombinedImageSampler
                           : readOnly ? ShaderResourceKind::SampledImage
                                      : ShaderResourceKind::StorageImage;
                break;
            case SLANG_ACCELERATION_STRUCTURE:
                resolved = ShaderResourceKind::AccelerationStructure;
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
        if (!resolved)
            throw std::runtime_error(reflectionError());

        result.bindings.push_back({paramName, static_cast<uint32_t>(set),
                                   static_cast<uint32_t>(binding),
                                   static_cast<uint32_t>(count), *resolved});
    }
    return result;
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

void SlangShaderCompiler::validateSchemas() {
    const auto program = _impl->load(_sourceDir, "reflect");
    Slang::ComPtr<slang::IBlob> diagnostic;
    auto *layout = program.linked->getLayout(0, diagnostic.writeRef());
    if (!layout)
        throw std::runtime_error("Slang: cannot reflect scene schema\n" + Impl::diagnostics(diagnostic));
    // Every Slang field must have a C++ member at the same offset. Checking the
    // Slang side exhaustively, rather than a sample, is what catches a swap of
    // two adjacent fields of the same size - a stride comparison alone cannot.
    // The C++ mirrors carry explicit padding members that Slang derives from
    // its own alignment rules, so extra C++ members are expected and ignored.
    const auto check = [](slang::ProgramLayout *reflection, const char *name, size_t cppSize,
                          slang::LayoutRules rules, std::initializer_list<Field> mirror) {
        auto *type = reflection->findTypeByName(name);
        auto *typeLayout = type ? reflection->getTypeLayout(type, rules) : nullptr;
        if (!typeLayout)
            throw std::runtime_error("Slang schema reflection omitted " + std::string(name));
        const auto mismatch = [name](const std::string &detail) {
            return std::runtime_error("Slang schema mismatch: " + std::string(name) + detail);
        };
        std::string undeclared;
        std::vector<const char *> reflected;
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
            reflected.push_back(fieldName);
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
        for (const auto &candidate : mirror) {
            if (std::find_if(reflected.begin(), reflected.end(), [candidate](const char *fieldName) {
                    return std::strcmp(candidate.name, fieldName) == 0;
                }) == reflected.end())
                throw mismatch(" C++ mirror declares " + std::string(candidate.name) + ", which Slang does not");
        }
        if (checked != mirror.size())
            throw mismatch(" reflected " + std::to_string(checked) + " named fields against " +
                           std::to_string(mirror.size()) + " in the C++ mirror");
        const auto stride = typeLayout->getStride(slang::ParameterCategory::Uniform);
        if (stride != cppSize)
            throw mismatch(" has stride " + std::to_string(cppSize) + " in C++, " + std::to_string(stride) + " in Slang");
    };
#define REONE_SCHEMA_FIELD(type, field) \
    Field { #field, offsetof(type, field) }
#define REONE_STORAGE_LAYOUT slang::LayoutRules::DefaultStructuredBuffer
    check(layout, "InstanceMaterial", sizeof(InstanceMaterial),
          REONE_STORAGE_LAYOUT,
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
           REONE_SCHEMA_FIELD(InstanceMaterial, envMapDerivedLayer),
           REONE_SCHEMA_FIELD(InstanceMaterial, alphaTest)});
    check(layout, "Matrix3x4", sizeof(Matrix3x4),
          REONE_STORAGE_LAYOUT,
          {REONE_SCHEMA_FIELD(Matrix3x4, row0),
           REONE_SCHEMA_FIELD(Matrix3x4, row1),
           REONE_SCHEMA_FIELD(Matrix3x4, row2)});
    check(layout, "MergedVertex", sizeof(MergedVertex),
          REONE_STORAGE_LAYOUT,
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
    check(layout, "SceneObject", sizeof(SceneObject),
          REONE_STORAGE_LAYOUT,
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
            REONE_SCHEMA_FIELD(SceneObject, dstCardBase),
            REONE_SCHEMA_FIELD(SceneObject, cardCount),
            REONE_SCHEMA_FIELD(SceneObject, geometryIndex),
            REONE_SCHEMA_FIELD(SceneObject, boneBase),
            REONE_SCHEMA_FIELD(SceneObject, boneCount),
            REONE_SCHEMA_FIELD(SceneObject, materialIndex),
            REONE_SCHEMA_FIELD(SceneObject, danglyBase),
            REONE_SCHEMA_FIELD(SceneObject, saberDisplacement)});
    check(layout, "GrassCardInstance", sizeof(GrassCardInstance),
          REONE_STORAGE_LAYOUT,
          {REONE_SCHEMA_FIELD(GrassCardInstance, row0),
           REONE_SCHEMA_FIELD(GrassCardInstance, row1),
           REONE_SCHEMA_FIELD(GrassCardInstance, row2),
           REONE_SCHEMA_FIELD(GrassCardInstance, prevRow0),
           REONE_SCHEMA_FIELD(GrassCardInstance, prevRow1),
           REONE_SCHEMA_FIELD(GrassCardInstance, prevRow2),
           REONE_SCHEMA_FIELD(GrassCardInstance, lightmapVariantCulled),
           REONE_SCHEMA_FIELD(GrassCardInstance, materialIndex)});
    check(layout, "ProceduralQuad", sizeof(ProceduralQuad),
          REONE_STORAGE_LAYOUT,
          {REONE_SCHEMA_FIELD(ProceduralQuad, positionVariant),
           REONE_SCHEMA_FIELD(ProceduralQuad, right),
           REONE_SCHEMA_FIELD(ProceduralQuad, up),
           REONE_SCHEMA_FIELD(ProceduralQuad, uvOffsetScale),
           REONE_SCHEMA_FIELD(ProceduralQuad, lightmapUV),
           REONE_SCHEMA_FIELD(ProceduralQuad, pad),
           REONE_SCHEMA_FIELD(ProceduralQuad, color)});
    check(layout, "GrassFace", sizeof(GrassFace),
          REONE_STORAGE_LAYOUT,
          {REONE_SCHEMA_FIELD(GrassFace, vertex0Uv0x),
           REONE_SCHEMA_FIELD(GrassFace, vertex1Uv0y),
           REONE_SCHEMA_FIELD(GrassFace, vertex2Uv1x),
           REONE_SCHEMA_FIELD(GrassFace, uv1yUv2QuadSize),
           REONE_SCHEMA_FIELD(GrassFace, probabilities),
           REONE_SCHEMA_FIELD(GrassFace, boundsMin),
           REONE_SCHEMA_FIELD(GrassFace, boundsMax),
           REONE_SCHEMA_FIELD(GrassFace, faceBudgetMaterialVariants)});
    check(layout, "GrassRange", sizeof(GrassRange),
          REONE_STORAGE_LAYOUT,
          {REONE_SCHEMA_FIELD(GrassRange, faceIndex),
           REONE_SCHEMA_FIELD(GrassRange, clusterOffset),
           REONE_SCHEMA_FIELD(GrassRange, clusterCount),
           REONE_SCHEMA_FIELD(GrassRange, pad)});
#undef REONE_STORAGE_LAYOUT

    const auto uniformProgram = _impl->load(_sourceDir, "reflect");
    diagnostic.setNull();
    auto *uniformLayout = uniformProgram.linked->getLayout(0, diagnostic.writeRef());
    if (!uniformLayout)
        throw std::runtime_error("Slang: cannot reflect uniforms\n" + Impl::diagnostics(diagnostic));
    const auto checkUniform = [uniformLayout, &check](const char *name, size_t cppSize,
                                                       std::initializer_list<Field> mirror) {
        // ConstantBuffer is the std140 ABI used by the Vulkan uniform ring. Do
        // not use the storage-buffer rule the scene tables require here.
        check(uniformLayout, name, cppSize, slang::LayoutRules::DefaultConstantBuffer, mirror);
    };
#define REONE_UNIFORM_FIELD(type, field) \
    Field { #field, offsetof(type, field) }
    checkUniform("GlobalUniforms", sizeof(GlobalUniforms),
                 {REONE_UNIFORM_FIELD(GlobalUniforms, projection),
                  REONE_UNIFORM_FIELD(GlobalUniforms, projectionInv),
                  REONE_UNIFORM_FIELD(GlobalUniforms, view),
                  REONE_UNIFORM_FIELD(GlobalUniforms, viewInv),
                  REONE_UNIFORM_FIELD(GlobalUniforms, cameraPosition),
                  REONE_UNIFORM_FIELD(GlobalUniforms, worldAmbientColor),
                  REONE_UNIFORM_FIELD(GlobalUniforms, lights),
                  REONE_UNIFORM_FIELD(GlobalUniforms, shadowLights),
                  REONE_UNIFORM_FIELD(GlobalUniforms, shadowCascadeFarPlanes),
                  REONE_UNIFORM_FIELD(GlobalUniforms, shadowCascadeSpace),
                  REONE_UNIFORM_FIELD(GlobalUniforms, viewProjection),
                  REONE_UNIFORM_FIELD(GlobalUniforms, prevViewProjection),
                  REONE_UNIFORM_FIELD(GlobalUniforms, fogColor),
                  REONE_UNIFORM_FIELD(GlobalUniforms, jitter),
                  REONE_UNIFORM_FIELD(GlobalUniforms, clipNear),
                  REONE_UNIFORM_FIELD(GlobalUniforms, clipFar),
                  REONE_UNIFORM_FIELD(GlobalUniforms, numLights),
                  REONE_UNIFORM_FIELD(GlobalUniforms, numShadowLights),
                  REONE_UNIFORM_FIELD(GlobalUniforms, fogNear),
                  REONE_UNIFORM_FIELD(GlobalUniforms, fogFar),
                  REONE_UNIFORM_FIELD(GlobalUniforms, time),
                  REONE_UNIFORM_FIELD(GlobalUniforms, prevTime)});
    checkUniform("LocalUniforms", sizeof(LocalUniforms),
                 {REONE_UNIFORM_FIELD(LocalUniforms, model),
                  REONE_UNIFORM_FIELD(LocalUniforms, modelInv),
                  REONE_UNIFORM_FIELD(LocalUniforms, prevModel),
                  REONE_UNIFORM_FIELD(LocalUniforms, uv),
                  REONE_UNIFORM_FIELD(LocalUniforms, color),
                  REONE_UNIFORM_FIELD(LocalUniforms, ambientColor),
                  REONE_UNIFORM_FIELD(LocalUniforms, diffuseColor),
                  REONE_UNIFORM_FIELD(LocalUniforms, selfIllumColor),
                  REONE_UNIFORM_FIELD(LocalUniforms, saberDisplacement),
                  REONE_UNIFORM_FIELD(LocalUniforms, featureMask),
                  REONE_UNIFORM_FIELD(LocalUniforms, bumpMapFrame),
                  REONE_UNIFORM_FIELD(LocalUniforms, bumpMapScale),
                  REONE_UNIFORM_FIELD(LocalUniforms, waterAlpha),
                  REONE_UNIFORM_FIELD(LocalUniforms, billboardSize),
                  REONE_UNIFORM_FIELD(LocalUniforms, envMapDerivedLayer),
                  REONE_UNIFORM_FIELD(LocalUniforms, iblRoughness)});
    checkUniform("BoneUniforms", sizeof(BoneUniforms),
                 {REONE_UNIFORM_FIELD(BoneUniforms, bones),
                  REONE_UNIFORM_FIELD(BoneUniforms, prevBones)});
    checkUniform("DanglyUniforms", sizeof(DanglyUniforms),
                 {REONE_UNIFORM_FIELD(DanglyUniforms, positions)});
    checkUniform("AABBUniforms", sizeof(AABBUniforms),
                 {REONE_UNIFORM_FIELD(AABBUniforms, corners)});
    checkUniform("ParticleUniforms", sizeof(ParticleUniforms),
                 {REONE_UNIFORM_FIELD(ParticleUniforms, gridSize),
                  REONE_UNIFORM_FIELD(ParticleUniforms, particles)});
    checkUniform("GrassUniforms", sizeof(GrassUniforms),
                 {REONE_UNIFORM_FIELD(GrassUniforms, quadSize),
                  REONE_UNIFORM_FIELD(GrassUniforms, radius),
                  REONE_UNIFORM_FIELD(GrassUniforms, clusters)});
    checkUniform("WalkmeshUniforms", sizeof(WalkmeshUniforms),
                 {REONE_UNIFORM_FIELD(WalkmeshUniforms, materials)});
    checkUniform("TextUniforms", sizeof(TextUniforms),
                 {REONE_UNIFORM_FIELD(TextUniforms, chars)});
    checkUniform("ScreenEffectUniforms", sizeof(ScreenEffectUniforms),
                 {REONE_UNIFORM_FIELD(ScreenEffectUniforms, projection),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, projectionInv),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, screenProjection),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, ssaoSamples),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, screenResolution),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, screenResolutionRcp),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, blurDirection),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, clipNear),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, clipFar),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, ssaoSampleRadius),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, ssaoBias),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, ssrBias),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, ssrPixelStride),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, ssrMaxSteps),
                  REONE_UNIFORM_FIELD(ScreenEffectUniforms, sharpenAmount)});
    checkUniform("GlobalUniformsLight", sizeof(GlobalUniformsLight),
                 {REONE_UNIFORM_FIELD(GlobalUniformsLight, position),
                  REONE_UNIFORM_FIELD(GlobalUniformsLight, color),
                  REONE_UNIFORM_FIELD(GlobalUniformsLight, multiplier),
                  REONE_UNIFORM_FIELD(GlobalUniformsLight, radius),
                  REONE_UNIFORM_FIELD(GlobalUniformsLight, ambientOnly),
                  REONE_UNIFORM_FIELD(GlobalUniformsLight, dynamicType),
                  REONE_UNIFORM_FIELD(GlobalUniformsLight, shadowSlot)});
    checkUniform("ParticleUniformsParticle", sizeof(ParticleUniformsParticle),
                 {REONE_UNIFORM_FIELD(ParticleUniformsParticle, positionFrame),
                  REONE_UNIFORM_FIELD(ParticleUniformsParticle, right),
                  REONE_UNIFORM_FIELD(ParticleUniformsParticle, up),
                  REONE_UNIFORM_FIELD(ParticleUniformsParticle, color),
                  REONE_UNIFORM_FIELD(ParticleUniformsParticle, size)});
    checkUniform("GrassUniformsCluster", sizeof(GrassUniformsCluster),
                 {REONE_UNIFORM_FIELD(GrassUniformsCluster, positionVariant),
                  REONE_UNIFORM_FIELD(GrassUniformsCluster, lightmapUV),
                  REONE_UNIFORM_FIELD(GrassUniformsCluster, yaw)});
    checkUniform("TextUniformsCharacter", sizeof(TextUniformsCharacter),
                 {REONE_UNIFORM_FIELD(TextUniformsCharacter, posScale),
                  REONE_UNIFORM_FIELD(TextUniformsCharacter, uv)});
#undef REONE_UNIFORM_FIELD
#undef REONE_SCHEMA_FIELD
}

} // namespace reone::graphics
