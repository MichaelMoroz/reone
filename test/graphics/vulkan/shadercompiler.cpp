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

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "reone/graphics/vulkan/shadercompiler.h"

namespace reone::graphics {

TEST(SlangShaderCompiler, compiles_engine_modules_and_validates_schemas) {
    SlangShaderCompiler compiler {REONE_SHADER_SOURCE_DIR};
    compiler.init();
    EXPECT_FALSE(compiler.module("scene_draw").empty());
    EXPECT_FALSE(compiler.module("path_trace").empty());
    EXPECT_TRUE(compiler.recompileAll());
    EXPECT_NO_THROW(compiler.validateSchemas());
    compiler.deinit();
}

TEST(SlangShaderCompiler, preserves_unbounded_descriptor_arrays) {
    SlangShaderCompiler compiler {REONE_SHADER_SOURCE_DIR};
    compiler.init();
    const auto reflection = compiler.reflection("path_trace");
    // The load path links a module without naming an entry point, so the
    // reflected stage is advisory - Unknown unless exactly one entry point
    // fixes it. What this test is about is the unbounded arrays below, which
    // reflection must not collapse to a fixed size.
    EXPECT_TRUE(reflection.stage == ShaderStage::RayGeneration ||
                reflection.stage == ShaderStage::Unknown);
    const auto find = [&reflection](const char *name) {
        return std::find_if(reflection.bindings.begin(), reflection.bindings.end(),
                            [name](const auto &binding) { return binding.name == name; });
    };
    const auto textures = find("bindlessTextures");
    const auto arrays = find("bindlessTextureArrays");
    ASSERT_NE(textures, reflection.bindings.end());
    ASSERT_NE(arrays, reflection.bindings.end());
    EXPECT_EQ(textures->count, 0u);
    EXPECT_EQ(arrays->count, 0u);
    compiler.deinit();
}

class TemporaryShaderSources {
public:
    TemporaryShaderSources() {
        _path = std::filesystem::temp_directory_path() / "reone" /
                ("slang-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::copy(REONE_SHADER_SOURCE_DIR, _path,
                              std::filesystem::copy_options::recursive);
    }

    ~TemporaryShaderSources() {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
    }

    const std::filesystem::path &path() const { return _path; }

private:
    std::filesystem::path _path;
};

TEST(SlangShaderCompiler, measures_cold_and_warm_startup) {
    TemporaryShaderSources sources;
    // A unique comment gives this run its own source hash, so the first init is
    // genuinely cold even when a developer already has a shader cache.
    std::ofstream {sources.path() / "common.slang", std::ios::app}
        << "\n// cold-warm test "
        << std::chrono::steady_clock::now().time_since_epoch().count() << '\n';

    const auto coldStart = std::chrono::steady_clock::now();
    {
        SlangShaderCompiler compiler {sources.path()};
        compiler.init();
        compiler.deinit();
    }
    const auto cold = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - coldStart);

    const auto warmStart = std::chrono::steady_clock::now();
    {
        SlangShaderCompiler compiler {sources.path()};
        compiler.init();
        compiler.deinit();
    }
    const auto warm = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - warmStart);

    std::cout << "Slang startup: cold=" << cold.count() << " ms, warm="
              << warm.count() << " ms\n";
    EXPECT_LT(warm, cold);
}

TEST(SlangShaderCompiler, names_the_field_for_a_mismatched_cpp_mirror) {
    TemporaryShaderSources sources;
    const auto schemaPath = sources.path() / "lib" / "scene_schema.slang";
    std::ifstream input(schemaPath);
    std::string schema {std::istreambuf_iterator<char> {input}, {}};
    const auto needle = "public uint envMap;";
    const auto position = schema.find(needle);
    ASSERT_NE(position, std::string::npos);
    // This models a C++ material mirror with the historical, incorrect layout:
    // the next field no longer has the reflected C++ offset.
    schema.replace(position, std::strlen(needle),
                   "public uint schemaTestPadding;\n    public uint envMap;");
    std::ofstream {schemaPath, std::ios::trunc} << schema;

    SlangShaderCompiler compiler {sources.path()};
    compiler.init();
    try {
        compiler.validateSchemas();
        FAIL() << "expected a schema-layout failure";
    } catch (const std::runtime_error &error) {
        EXPECT_NE(std::string {error.what()}.find("InstanceMaterial::envMap"),
                  std::string::npos);
    }
    compiler.deinit();
}

TEST(SlangShaderCompiler, retains_a_last_good_module_after_a_source_error) {
    TemporaryShaderSources sources;
    SlangShaderCompiler compiler {sources.path()};
    compiler.init();
    const auto lastGood = compiler.module("scene_draw");

    std::ofstream {sources.path() / "lib" / "scene_schema.slang", std::ios::app}
        << "\nthis is deliberately invalid Slang\n";

    const auto &retained = compiler.module("scene_draw");
    EXPECT_EQ(retained, lastGood);
    compiler.deinit();
}

TEST(SlangShaderCompiler, recompiles_a_changed_module_at_runtime) {
    TemporaryShaderSources sources;
    SlangShaderCompiler compiler {sources.path()};
    compiler.init();
    const auto before = compiler.module("scene_draw");

    const auto shaderPath = sources.path() / "scene_draw.slang";
    std::ifstream input(shaderPath);
    std::string shader {std::istreambuf_iterator<char> {input}, {}};
    const auto needle = "static const uint kMegaFeatureLightmap = 1u << 0;";
    const auto position = shader.find(needle);
    ASSERT_NE(position, std::string::npos);
    // This only exists in the test source copy. It is a valid shader edit that
    // changes emitted code and represents the edit-reload loop in the engine.
    shader.replace(position, std::strlen(needle),
                   "static const uint kMegaFeatureLightmap = 1u << 30;");
    std::ofstream {shaderPath, std::ios::trunc} << shader;

    EXPECT_TRUE(compiler.recompileAll());
    EXPECT_NE(compiler.module("scene_draw"), before);
    compiler.deinit();
}

} // namespace reone::graphics
