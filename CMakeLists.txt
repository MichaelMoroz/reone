# Copyright (c) 2020-2023 The reone project contributors

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.16)
project(reone)

set(CMAKE_CXX_STANDARD 17)
set(CLANG_FORMAT_PATH ${CMAKE_SOURCE_DIR}/.clang-format)

# Options

option(BUILD_TESTS "build tests" ON)
option(BUILD_LAUNCHER "build launcher application" ON)
option(BUILD_TOOLKIT "build toolkit application" ON)
option(BUILD_DATAMINER "build dataminer application" ON)
option(BUILD_GFF2JSON "build gff2json application" ON)
option(BUILD_UNERF "build unerf application" ON)

option(ENABLE_MOVIE "enable movie playback" ON)
option(ENABLE_VULKAN "build the Vulkan backend" OFF)
option(ENABLE_ASAN "enable address sanitizer" OFF)

# NRD ships under the proprietary NVIDIA RTX SDKs license, which both forbids
# and is forbidden by GPL-3 in a distributed binary. It is therefore strictly
# a local developer toggle: OFF by default, fetched and linked only when
# enabled, and builds without it must remain fully functional.
option(ENABLE_NRD "fetch and link the NVIDIA Real-time Denoisers SDK (local builds only)" OFF)

# FidelityFX is MIT, so unlike NRD this one is distributable. It stays off by
# default because its committed shader compiler is Windows-only; builds without
# it must remain fully functional.
option(ENABLE_FSR "fetch and link the AMD FidelityFX Super Resolution 2 upscaler" OFF)

# END Options

# Dependencies

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
find_package(Boost REQUIRED COMPONENTS program_options exception json)
find_package(OpenGL REQUIRED)
find_package(MAD REQUIRED)

if(MSVC)
    find_package(OpenAL CONFIG REQUIRED)
    find_package(SDL3 CONFIG REQUIRED)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OpenAL REQUIRED openal)
    pkg_check_modules(SDL3 REQUIRED sdl3)
    find_package(Threads REQUIRED)
endif()

if(ENABLE_MOVIE)
    if(MSVC)
        find_package(FFMPEG REQUIRED)
    else()
        pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswresample libswscale)
    endif()
    include_directories(${FFMPEG_INCLUDE_DIRS})
endif()

if(BUILD_LAUNCHER OR BUILD_TOOLKIT)
    find_package(wxWidgets REQUIRED core base adv stc aui)
    include(${wxWidgets_USE_FILE})
endif()

find_package(imgui CONFIG REQUIRED)

if(ENABLE_VULKAN)
    # Headers only: volk loads every entry point at runtime, so nothing links
    # vulkan-1 and the build does not depend on a loader being present.
    find_package(VulkanHeaders CONFIG REQUIRED)
    find_package(volk CONFIG REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    find_package(vk-bootstrap CONFIG REQUIRED)
endif()

if(ENABLE_NRD)
    if(NOT ENABLE_VULKAN)
        message(FATAL_ERROR "ENABLE_NRD requires ENABLE_VULKAN")
    endif()
    # NRD is GAPI-free: it embeds SPIRV compute shaders and hands back dispatch
    # descriptions that the ray-query pipeline records itself. No NRI - its own
    # Vulkan entry-point resolution would collide with volk's ownership of the
    # vk* symbols, the same silent-crash class as the VMA case.
    include(FetchContent)
    FetchContent_Declare(NRD
        GIT_REPOSITORY https://github.com/NVIDIA-RTX/NRD.git
        GIT_TAG v4.17.3
        GIT_SHALLOW TRUE)
    set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_SPIRV_SHADERS ON CACHE BOOL "" FORCE)
    # RGBA16_SNORM normal encoding (nrd::NormalEncoding), chosen because it
    # accepts plain FP textures: the tracer writes world normal and linear
    # roughness into RGBA16F unpacked, no oct-encoding shader helpers needed.
    set(NRD_NORMAL_ENCODING "4" CACHE STRING "" FORCE)
    set(NRD_EMBEDS_DXIL_SHADERS OFF CACHE BOOL "" FORCE)
    set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
    set(NRD_DISABLE_SHADER_COMPILATION OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(NRD)
    add_compile_definitions(R_ENABLE_NRD)
endif()

if(ENABLE_FSR)
    if(NOT ENABLE_VULKAN)
        message(FATAL_ERROR "ENABLE_FSR requires ENABLE_VULKAN")
    endif()
    if(NOT MSVC)
        message(FATAL_ERROR "ENABLE_FSR requires MSVC: FSR2's committed shader compiler is Windows-only")
    endif()

    # FSR2's project assumes it is the top-level project and writes archives to
    # CMAKE_HOME_DIRECTORY. Fetch only its source and own the small static
    # library here so generated data remains in the build tree.
    include(FetchContent)
    FetchContent_Declare(FSR2
        GIT_REPOSITORY https://github.com/GPUOpen-Effects/FidelityFX-FSR2.git
        GIT_TAG v2.2.1
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(FSR2)
    if(NOT fsr2_POPULATED)
        FetchContent_Populate(FSR2)
    endif()

    set(FSR2_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/fsr2)
    set(FSR2_API_DIR ${fsr2_SOURCE_DIR}/src/ffx-fsr2-api)
    set(FSR2_SHADER_DIR ${FSR2_API_DIR}/shaders)
    file(GLOB FSR2_SHADER_DEPENDENCIES ${FSR2_SHADER_DIR}/*.glsl ${FSR2_SHADER_DIR}/*.h)

    # These argument sets are upstream's, reproduced rather than invented: see
    # FFX_SC_BASE_ARGS / FFX_SC_VK_BASE_ARGS / FFX_SC_PERMUTATION_ARGS in the
    # SDK's own src/ffx-fsr2-api CMakeLists. The half-precision sampler dials
    # are not permutation axes - only reprojection does half - and omitting any
    # of them, or the -name= the generated header keys off, makes the compiler
    # fail with an empty error message.
    set(FSR2_SC_BASE_ARGS
        -reflection -deps=gcc -DFFX_GPU=1
        -DFFX_FSR2_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FSR2_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FSR2_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
        -DFFX_FSR2_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
        -DFFX_FSR2_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2)
    set(FSR2_SC_VK_ARGS -compiler=glslang -e main --target-env vulkan1.1 -S comp -Os -DFFX_GLSL=1)
    set(FSR2_SC_PERMUTATION_ARGS
        -DFFX_FSR2_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}
        -DFFX_FSR2_OPTION_HDR_COLOR_INPUT={0,1}
        -DFFX_FSR2_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}
        -DFFX_FSR2_OPTION_JITTERED_MOTION_VECTORS={0,1}
        -DFFX_FSR2_OPTION_INVERTED_DEPTH={0,1}
        -DFFX_FSR2_OPTION_APPLY_SHARPENING={0,1})

    set(FSR2_SHADER_HEADERS)
    function(add_fsr2_shader_permutations pass)
        set(name ffx_fsr2_${pass}_pass)
        set(source ${FSR2_SHADER_DIR}/${name}.glsl)
        set(output ${FSR2_SHADER_OUTPUT_DIR}/${name}_permutations.h)
        # The luminance pyramid pass has no half-precision variant.
        set(half -DFFX_HALF={0,1})
        if(pass STREQUAL "compute_luminance_pyramid")
            set(half -DFFX_HALF=0)
        endif()
        add_custom_command(
            OUTPUT ${output}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${FSR2_SHADER_OUTPUT_DIR}
            COMMAND ${fsr2_SOURCE_DIR}/tools/sc/FidelityFX_SC.exe
                ${FSR2_SC_BASE_ARGS} ${FSR2_SC_VK_ARGS} ${FSR2_SC_PERMUTATION_ARGS} ${half}
                -name=${name} -I${FSR2_SHADER_DIR}
                -output=${FSR2_SHADER_OUTPUT_DIR} ${source}
            DEPENDS ${source} ${FSR2_SHADER_DEPENDENCIES}
            COMMENT "FidelityFX_SC ${name}"
            VERBATIM)
        list(APPEND FSR2_SHADER_HEADERS ${output})
        set(FSR2_SHADER_HEADERS ${FSR2_SHADER_HEADERS} PARENT_SCOPE)
    endfunction()
    foreach(pass tcr_autogen autogen_reactive accumulate compute_luminance_pyramid depth_clip lock
                 reconstruct_previous_depth rcas)
        add_fsr2_shader_permutations(${pass})
    endforeach()

    add_custom_target(fsr2_shaders DEPENDS ${FSR2_SHADER_HEADERS})
    add_library(fsr2 STATIC
        ${fsr2_SOURCE_DIR}/src/ffx-fsr2-api/ffx_fsr2.cpp
        ${fsr2_SOURCE_DIR}/src/ffx-fsr2-api/ffx_assert.cpp
        ${fsr2_SOURCE_DIR}/src/ffx-fsr2-api/vk/ffx_fsr2_vk.cpp
        ${fsr2_SOURCE_DIR}/src/ffx-fsr2-api/vk/shaders/ffx_fsr2_shaders_vk.cpp
        ${FSR2_SHADER_HEADERS})
    add_dependencies(fsr2 fsr2_shaders)
    # The public headers live beside the sources, not in a separate include/.
    target_include_directories(fsr2 PUBLIC ${FSR2_API_DIR} ${FSR2_API_DIR}/vk
                                  PRIVATE ${FSR2_SHADER_OUTPUT_DIR})
    # volk owns Vulkan entry points in this process. Force it in before FSR2's
    # Vulkan header so direct backend calls resolve through volk's loaded table.
    target_compile_definitions(fsr2 PRIVATE FFX_GCC VK_NO_PROTOTYPES VOLK_NAMESPACE)
    target_compile_options(fsr2 PRIVATE /FIvolk.h)
    target_link_libraries(fsr2 PRIVATE Vulkan::Headers volk::volk_headers)
    add_compile_definitions(R_ENABLE_FSR)
endif()

# Compiles slang/ to SPIR-V for Vulkan and regenerates the uniform layout
# assertions from GLSL-target reflection. Both uses degrade gracefully when it
# is missing: Vulkan shader modules are absent, and the assertion header is committed.
find_program(SLANGC_EXECUTABLE slangc
    HINTS ${CMAKE_PREFIX_PATH} ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shader-slang
    DOC "Slang compiler")
if(SLANGC_EXECUTABLE)
    message(STATUS "Found slangc: ${SLANGC_EXECUTABLE}")
else()
    message(STATUS "slangc not found - Vulkan shader modules will not be transpiled")
endif()

set(REONE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/include)
set(GLAD_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/extern/glad/include)
include_directories(${Boost_INCLUDE_DIRS} ${SDL3_INCLUDE_DIRS} ${MAD_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/extern/glm ${REONE_INCLUDE_DIR} ${GLAD_INCLUDE_DIR})

add_compile_definitions(BOOST_BIND_GLOBAL_PLACEHOLDERS)

# GLM GTX extensions (euler_angles et al.) are gated behind this define in
# recent GLM releases; required when a system/vcpkg GLM shadows extern/glm.
add_compile_definitions(GLM_ENABLE_EXPERIMENTAL)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

    # The Visual Studio generator compiles a project's sources one at a time.
    # Fanning them across cores dominates every other build-time knob here,
    # because the work is concentrated in a few large libraries rather than
    # spread over many projects MSBuild could run in parallel.
    add_compile_options(/MP)
endif()

# END Dependencies

# Compile options

if(ENABLE_ASAN)
    if (MSVC)
        add_compile_options("/fsanitize=address")
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

# END Compile options

# Libraries

add_subdirectory(src/libs/system) # system static library
add_subdirectory(src/libs/resource) # resource static library
add_subdirectory(src/libs/graphics) # graphics static library

if(ENABLE_VULKAN)
    add_subdirectory(src/libs/graphics/vulkan) # vulkan backend static library
endif()
add_subdirectory(src/libs/audio) # audio static library
add_subdirectory(src/libs/input) # input static library
add_subdirectory(src/libs/json) # json static library
add_subdirectory(src/libs/gui) # gui static library
add_subdirectory(src/libs/scene) # scene static library
add_subdirectory(src/libs/movie) # movie static library
add_subdirectory(src/libs/script) # script static library
add_subdirectory(src/libs/game) # game static library
add_subdirectory(src/libs/tools) # tools static library

# END Libraries

# Applications

add_subdirectory(src/apps/shaderpack) # shaderpack application
add_subdirectory(src/apps/uniformgen) # uniform layout assertion generator
add_subdirectory(src/apps/engine) # engine application


if(BUILD_LAUNCHER)
    add_subdirectory(src/apps/launcher) # launcher application
endif()

if(BUILD_TOOLKIT)
    add_subdirectory(src/apps/toolkit) # toolkit application
endif()

if(BUILD_DATAMINER)
    add_subdirectory(src/apps/dataminer) # dataminer application
endif()

if(BUILD_GFF2JSON)
    add_subdirectory(src/apps/gff2json) # gff2json application
endif()

if(BUILD_UNERF)
    add_subdirectory(src/apps/unerf) # unpack ERF files
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(test) # tests executable
endif()

# END Applications

# Installation

if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
endif()

install(TARGETS ${InstallTargets} DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS system resource graphics audio scene movie script gui game tools DESTINATION ${CMAKE_INSTALL_LIBDIR})

# END Installation
