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
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace reone::graphics {

/**
 * Compiles the engine's Slang modules directly to SPIR-V.
 *
 * A module is cached on disk under a hash of every .slang source file and in
 * memory by module name.  The broad source hash intentionally favours a
 * correct warm start over trying to duplicate Slang's dependency resolver.
 * Failed recompiles leave the last successful module available to callers.
 */
class SlangShaderCompiler {
public:
    explicit SlangShaderCompiler(std::filesystem::path sourceDir);
    ~SlangShaderCompiler();

    SlangShaderCompiler(const SlangShaderCompiler &) = delete;
    SlangShaderCompiler &operator=(const SlangShaderCompiler &) = delete;

    void init();
    void deinit();
    void setSourceDir(std::filesystem::path sourceDir) { _sourceDir = std::move(sourceDir); }

    /** Returns a valid last-known-good module or throws on its first failure. */
    const std::vector<uint32_t> &module(const std::string &name);
    /** Compile every engine module now. False means at least one old module was retained. */
    bool recompileAll();
    /** Drops memory state so the next module request rechecks the disk cache. */
    void invalidate();

    /** Verifies the C++ GPU scene and uniform mirrors against their Slang layouts. */
    void validateSchemas();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    std::filesystem::path _sourceDir;
    std::filesystem::path _cacheDir;
    std::unordered_map<std::string, std::vector<uint32_t>> _modules;
    std::unordered_map<std::string, uint64_t> _moduleHashes;
    uint64_t _sourceHash {0};
    size_t _cacheHits {0};
    size_t _compiledModules {0};

    uint64_t sourceHash() const;
    std::vector<uint32_t> compile(const std::string &name, bool &fromCache, bool force = false);
};

} // namespace reone::graphics
