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

#include "reone/graphics/vulkan/device.h"
#include "reone/graphics/vulkan/shadercompiler.h"
#ifdef R_ENABLE_FSR
#include "shaders/ffx_fsr2_shaders_vk.h"
#endif

namespace reone::graphics {
namespace {

constexpr uint32_t kSpirvOpCapability = 17;
constexpr uint32_t kSpirvCapabilityShader = 1;
constexpr uint32_t kSpirvCapabilityFloat16 = 9;
constexpr uint32_t kSpirvCapabilityInt64 = 11;
constexpr uint32_t kSpirvCapabilityInt16 = 22;
constexpr uint32_t kSpirvCapabilityStorageImageExtendedFormats = 49;
constexpr uint32_t kSpirvCapabilityImageQuery = 50;
constexpr uint32_t kSpirvCapabilityStorageImageReadWithoutFormat = 55;
constexpr uint32_t kSpirvCapabilityStorageImageWriteWithoutFormat = 56;
constexpr uint32_t kSpirvCapabilityGroupNonUniform = 61;
constexpr uint32_t kSpirvCapabilityGroupNonUniformQuad = 68;
constexpr uint32_t kSpirvCapabilityRayQuery = 4472;
constexpr uint32_t kSpirvCapabilityRayTracing = 4479;
constexpr uint32_t kSpirvCapabilityShaderNonUniform = 5301;
constexpr uint32_t kSpirvCapabilityRuntimeDescriptorArray = 5302;

std::vector<uint32_t> declaredCapabilities(const std::vector<uint32_t> &words) {
    std::vector<uint32_t> result;
    if (words.size() < 5) return result;
    for (size_t offset = 5; offset < words.size();) {
        const uint32_t wordCount = words[offset] >> 16;
        const uint32_t opcode = words[offset] & 0xffffu;
        if (wordCount == 0 || wordCount > words.size() - offset) return {};
        if (opcode == kSpirvOpCapability && wordCount >= 2)
            result.push_back(words[offset + 1]);
        offset += wordCount;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace

TEST(VulkanDeviceFeatures, fsr_embedded_spirv_matches_the_enabled_width_features) {
    VkPhysicalDeviceFeatures supportedCore {};
    supportedCore.shaderInt16 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features supportedVulkan12 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    supportedVulkan12.shaderFloat16 = VK_TRUE;
    const auto features = VulkanDevice::fsrFeatures(supportedCore, supportedVulkan12);
    EXPECT_TRUE(features.available);
    EXPECT_EQ(features.core.shaderInt16, VK_TRUE);
    EXPECT_EQ(features.vulkan12.shaderFloat16, VK_TRUE);

    supportedCore.shaderInt16 = VK_FALSE;
    const auto splitSupport = VulkanDevice::fsrFeatures(supportedCore, supportedVulkan12);
    EXPECT_FALSE(splitSupport.available);
    EXPECT_EQ(splitSupport.core.shaderInt16, VK_FALSE);
    EXPECT_EQ(splitSupport.vulkan12.shaderFloat16, VK_FALSE);

    supportedVulkan12.shaderFloat16 = VK_FALSE;
    const auto fp32Fallback = VulkanDevice::fsrFeatures(supportedCore, supportedVulkan12);
    EXPECT_TRUE(fp32Fallback.available);
    EXPECT_EQ(fp32Fallback.core.shaderInt16, VK_FALSE);
    EXPECT_EQ(fp32Fallback.vulkan12.shaderFloat16, VK_FALSE);

#ifdef R_ENABLE_FSR
    const uint32_t halfFlags = FSR2_SHADER_PERMUTATION_HDR_COLOR_INPUT |
                               FSR2_SHADER_PERMUTATION_LOW_RES_MOTION_VECTORS |
                               FSR2_SHADER_PERMUTATION_ALLOW_FP16;
    const auto capabilitiesFor = [halfFlags](FfxFsr2Pass pass) {
        const auto blob = fsr2GetPermutationBlobByIndexVK(pass, halfFlags);
        std::vector<uint32_t> words(blob.size / sizeof(uint32_t));
        std::memcpy(words.data(), blob.data, words.size() * sizeof(uint32_t));
        return declaredCapabilities(words);
    };
    for (int passValue = 0; passValue < FFX_FSR2_PASS_COUNT; ++passValue) {
        const auto pass = static_cast<FfxFsr2Pass>(passValue);
        for (const uint32_t capability : capabilitiesFor(pass)) {
            switch (capability) {
            case kSpirvCapabilityShader:
            case kSpirvCapabilityStorageImageExtendedFormats:
            case kSpirvCapabilityStorageImageWriteWithoutFormat:
            case kSpirvCapabilityGroupNonUniform:
            case kSpirvCapabilityGroupNonUniformQuad:
                break;
            case kSpirvCapabilityInt16:
                EXPECT_EQ(features.core.shaderInt16, VK_TRUE)
                    << "FSR pass " << passValue << " requires disabled Int16";
                break;
            case kSpirvCapabilityFloat16:
                EXPECT_EQ(features.vulkan12.shaderFloat16, VK_TRUE)
                    << "FSR pass " << passValue << " requires disabled Float16";
                break;
            default:
                ADD_FAILURE() << "FSR pass " << passValue
                              << " declares unmapped SPIR-V capability " << capability;
                break;
            }
        }
    }
    const auto requires = [&](FfxFsr2Pass pass, uint32_t capability,
                              VkBool32 enabled, const char *name) {
        const auto capabilities = capabilitiesFor(pass);
        EXPECT_NE(std::find(capabilities.begin(), capabilities.end(), capability),
                  capabilities.end())
            << "FSR pass " << static_cast<int>(pass) << " no longer declares " << name;
        EXPECT_EQ(enabled, VK_TRUE)
            << "FSR pass " << static_cast<int>(pass) << " requires disabled " << name;
    };
    requires(FFX_FSR2_PASS_RECONSTRUCT_PREVIOUS_DEPTH, kSpirvCapabilityInt16,
             features.core.shaderInt16, "Int16");
    requires(FFX_FSR2_PASS_GENERATE_REACTIVE, kSpirvCapabilityInt16,
             features.core.shaderInt16, "Int16");
    requires(FFX_FSR2_PASS_TCR_AUTOGENERATE, kSpirvCapabilityInt16,
             features.core.shaderInt16, "Int16");
    requires(FFX_FSR2_PASS_TCR_AUTOGENERATE, kSpirvCapabilityFloat16,
             features.vulkan12.shaderFloat16, "Float16");
#endif
}

TEST(SlangShaderCompiler, compiles_engine_modules_and_validates_schemas) {
    SlangShaderCompiler compiler {REONE_SHADER_SOURCE_DIR};
    compiler.init();
    EXPECT_FALSE(compiler.module("scene_draw").empty());
    EXPECT_FALSE(compiler.module("scene_resolve").empty());
    const std::array<const char *, 2> rayQueryModules {{"path_trace", "tracing_instances"}};
    for (const auto *module : rayQueryModules)
        EXPECT_FALSE(compiler.module(module).empty());

    VkPhysicalDeviceFeatures rasterCore {};
    rasterCore.robustBufferAccess = VK_TRUE;
    rasterCore.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkPhysicalDeviceVulkan12Features rasterVulkan12 {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    rasterVulkan12.runtimeDescriptorArray = VK_TRUE;
    const auto features = VulkanDevice::rayQueryFeatures(rasterCore, rasterVulkan12);
    const auto tracingInstanceCapabilities =
        declaredCapabilities(compiler.module("tracing_instances"));
    EXPECT_EQ(std::find(tracingInstanceCapabilities.begin(),
                        tracingInstanceCapabilities.end(),
                        kSpirvCapabilityInt64),
              tracingInstanceCapabilities.end())
        << "tracing_instances declares the SPIR-V Int64 capability";
    const auto hasExtension = [&features](const char *name) {
        return std::find_if(features.extensions.begin(), features.extensions.end(),
                            [name](const char *extension) {
                                return std::strcmp(extension, name) == 0;
                            }) != features.extensions.end();
    };
    struct CapabilityRequirement {
        uint32_t capability;
        const char *name;
        bool satisfied;
    };
    const std::array requirements {
        CapabilityRequirement {kSpirvCapabilityShader, "Shader",
                               features.apiVersion >= VK_API_VERSION_1_0},
        CapabilityRequirement {kSpirvCapabilityInt64, "Int64",
                               features.core.shaderInt64 == VK_TRUE},
        CapabilityRequirement {kSpirvCapabilityImageQuery, "ImageQuery",
                               features.apiVersion >= VK_API_VERSION_1_0},
        CapabilityRequirement {kSpirvCapabilityStorageImageReadWithoutFormat,
                               "StorageImageReadWithoutFormat",
                               features.apiVersion >= VK_API_VERSION_1_3 ||
                                   features.core.shaderStorageImageReadWithoutFormat == VK_TRUE},
        CapabilityRequirement {kSpirvCapabilityStorageImageWriteWithoutFormat,
                               "StorageImageWriteWithoutFormat",
                               features.apiVersion >= VK_API_VERSION_1_3 ||
                                   features.core.shaderStorageImageWriteWithoutFormat == VK_TRUE},
        CapabilityRequirement {kSpirvCapabilityRayQuery, "RayQueryKHR",
                               features.rayQuery.rayQuery == VK_TRUE &&
                                   hasExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME)},
        CapabilityRequirement {kSpirvCapabilityRayTracing, "RayTracingKHR",
                               features.rayTracingPipeline.rayTracingPipeline == VK_TRUE &&
                                   hasExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)},
        CapabilityRequirement {kSpirvCapabilityShaderNonUniform, "ShaderNonUniform",
                               features.apiVersion >= VK_API_VERSION_1_2},
        CapabilityRequirement {kSpirvCapabilityRuntimeDescriptorArray,
                               "RuntimeDescriptorArray",
                               features.vulkan12.runtimeDescriptorArray == VK_TRUE},
    };
    for (const auto *module : rayQueryModules) {
        for (const uint32_t capability : declaredCapabilities(compiler.module(module))) {
            const auto requirement = std::find_if(
                requirements.begin(), requirements.end(),
                [capability](const auto &candidate) {
                    return candidate.capability == capability;
                });
            ASSERT_NE(requirement, requirements.end())
                << module << " declares unmapped SPIR-V capability " << capability;
            EXPECT_TRUE(requirement->satisfied)
                << module << " requires disabled SPIR-V capability " << requirement->name;
        }
    }

    const auto rasterLogicalPhysicalDevice = VulkanDevice::prepareLogicalDevice(
        vkb::PhysicalDevice {}, rasterCore);
    EXPECT_EQ(rasterLogicalPhysicalDevice.features.shaderInt64, VK_FALSE);
    EXPECT_EQ(rasterLogicalPhysicalDevice.features.robustBufferAccess, VK_TRUE);

    vkb::PhysicalDevice selectedPhysicalDevice;
    const auto logicalPhysicalDevice = VulkanDevice::prepareLogicalDevice(
        std::move(selectedPhysicalDevice), features.core);
    EXPECT_EQ(logicalPhysicalDevice.features.shaderInt64, VK_FALSE);
    EXPECT_EQ(logicalPhysicalDevice.features.robustBufferAccess, VK_TRUE);
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

TEST(SlangShaderCompiler, reflects_push_constants_used_by_selected_entry_points) {
    SlangShaderCompiler compiler {REONE_SHADER_SOURCE_DIR};
    compiler.init();
    const auto reflectFragment = [&compiler](const char *fragment) {
        return compiler.reflection(
            "postprocess",
            {{"postVertex", ShaderStage::Vertex},
             {fragment, ShaderStage::Fragment}});
    };
    EXPECT_EQ(reflectFragment("postProcessFragment").pushConstantSize, 16u);
    EXPECT_EQ(reflectFragment("primaryCoverageFragment").pushConstantSize, 4u);
    EXPECT_EQ(reflectFragment("bloomCompositeFragment").pushConstantSize, 16u);
    // The composite's push block is the denoiser values alone: two scalars for
    // the jitter and two uints. Fog left it when it became a tail pass.
    EXPECT_EQ(compiler.reflection("composite", {{"main", ShaderStage::Compute}}).pushConstantSize,
              16u);
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
    const auto needle = "static const uint kMegaCoverageUngated = 0u;";
    const auto position = shader.find(needle);
    ASSERT_NE(position, std::string::npos);
    // This only exists in the test source copy. It is a valid shader edit that
    // changes emitted code and represents the edit-reload loop in the engine.
    shader.replace(position, std::strlen(needle),
                   "static const uint kMegaCoverageUngated = 7u;");
    std::ofstream {shaderPath, std::ios::trunc} << shader;

    EXPECT_TRUE(compiler.recompileAll());
    EXPECT_NE(compiler.module("scene_draw"), before);
    compiler.deinit();
}

} // namespace reone::graphics
