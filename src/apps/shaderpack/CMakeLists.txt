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

set(SHADERPACK_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/apps/shaderpack)
set(SHADERPACK_SOURCES ${SHADERPACK_SOURCE_DIR}/main.cpp)

add_executable(shaderpack ${SHADERPACK_SOURCES} ${CLANG_FORMAT_PATH})
set_target_properties(shaderpack PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}$<$<CONFIG:Debug>:/debug>/bin)
target_precompile_headers(shaderpack PRIVATE ${CMAKE_SOURCE_DIR}/src/pch.h)
target_link_libraries(shaderpack PRIVATE resource movie graphics audio script system ${Boost_PROGRAM_OPTIONS_LIBRARY} ${Boost_EXCEPTION_LIBRARY})

# Hand-written GLSL is staged into one flat directory before packing. SPIR-V
# modules are emitted beside the executable for Vulkan rather than packed.
set(SHADER_STAGE_DIR ${CMAKE_BINARY_DIR}/shaders)

# One SPIR-V module per source file, carrying every entry point that file
# declares. Binding is by number, so stages of a program need no agreement on
# names. Emitted beside the executable; the shader pack carries the hand-written
# GLSL used by OpenGL.
set(SPIRV_OUT_DIR ${CMAKE_BINARY_DIR}$<$<CONFIG:Debug>:/debug>/bin/spirv)

# The SER binary is separate: its SPIR-V requires the NV extension, while the
# ordinary ray-query binary must remain loadable on devices without it.
set(R_SER_HINT_MODE "2" CACHE STRING "SER hint: 1=hit, 2=material low 8 bits, 3=material plus hit")
set_property(CACHE R_SER_HINT_MODE PROPERTY STRINGS 1 2 3)
if(NOT R_SER_HINT_MODE MATCHES "^[123]$")
    message(FATAL_ERROR "R_SER_HINT_MODE must be 1, 2, or 3")
endif()

# <source>:<entry>[+<entry>...]
set(SPIRV_MODULES
    "pbr_model:staticVertex+skinnedVertex+danglyVertex+saberVertex+opaqueFragment+retroOpaqueFragment+transparentFragment+oitModelFragment+skyBakeFragment"
    "grass:grassVertex+pbrFragment+retroFragment"
    "walkmesh:walkmeshVertex+pbrFragment+retroFragment"
    "common:billboardVertex+textureFragment+retroAABBFragment"
    "shadow:shadowVertex+nullFragment+pointFragment"
    "pbr_ibl:iblVertex+brdfFragment+irradianceFragment+prefilterFragment"
    "particles:particleVertex+particleFragment+oitParticleFragment"
    "pbr_resolve:resolveVertex+resolveFragment"
    "pbr_ssao:ssaoVertex+ssaoFragment"
    "pbr_ssr:ssrVertex+ssrFragment"
    "rayquery:main"
    "skin:main"
    "nrd_composite:main"
    "pt_tonemap:main"
    "postprocess:postVertex+boxBlur4Fragment+gausBlur9Fragment+gausBlur13Fragment+sharpenFragment+fxaaFragment+oitBlendFragment+debugTextureFragment"
    "vk2d:quadVertex+imageFragment+colorFragment+fullTargetVertex+textVertex+textFragment")

# Every .slang file, not just the module being compiled. A module imports from
# slang/lib, and listing only the top-level file meant editing lib/hashedalpha
# or lib/math left the .spv untouched while the build reported success - a
# stale-module trap that has now cost three separate investigations, the last
# one measuring a "fix" that was never in the binary.
file(GLOB_RECURSE SLANG_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/slang/*.slang)

if(SLANGC_EXECUTABLE)
    set(SPIRV_OUTPUTS)
    foreach(entry ${SPIRV_MODULES})
        string(REPLACE ":" ";" parts ${entry})
        list(GET parts 0 spv_source)
        list(GET parts 1 spv_entries)
        string(REPLACE "+" ";" spv_entries "${spv_entries}")
        set(entry_args)
        foreach(e ${spv_entries})
            list(APPEND entry_args -entry ${e})
        endforeach()
        set(out ${SPIRV_OUT_DIR}/${spv_source}.spv)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SPIRV_OUT_DIR}
            COMMAND ${SLANGC_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/slang/${spv_source}.slang
                -I ${CMAKE_SOURCE_DIR}/slang
                -target spirv -matrix-layout-column-major
                -capability SPV_EXT_physical_storage_buffer
                ${entry_args} -o ${out}
            DEPENDS ${SLANG_SOURCES}
            COMMENT "slangc ${spv_source}.slang -> ${spv_source}.spv")
        list(APPEND SPIRV_OUTPUTS ${out})
    endforeach()
    set(rayquery_ser_out ${SPIRV_OUT_DIR}/rayquery_ser.spv)
    add_custom_command(
        OUTPUT ${rayquery_ser_out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SPIRV_OUT_DIR}
        COMMAND ${SLANGC_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/slang/rayquery.slang
            -I ${CMAKE_SOURCE_DIR}/slang
            -target spirv -matrix-layout-column-major
            -capability SPV_EXT_physical_storage_buffer
            -D R_ENABLE_SER=1 -D R_SER_HINT_MODE=${R_SER_HINT_MODE}
            -entry main -o ${rayquery_ser_out}
        DEPENDS ${SLANG_SOURCES}
        COMMENT "slangc rayquery.slang -> rayquery_ser.spv (SER hint ${R_SER_HINT_MODE})")
    list(APPEND SPIRV_OUTPUTS ${rayquery_ser_out})
    add_custom_target(compile_spirv ALL DEPENDS ${SPIRV_OUTPUTS})
else()
    add_custom_target(compile_spirv)
endif()

add_custom_target(create_shaderpack ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/glsl ${SHADER_STAGE_DIR}
    COMMAND shaderpack ${SHADER_STAGE_DIR} ${CMAKE_BINARY_DIR}$<$<CONFIG:Debug>:/debug>/bin
    DEPENDS shaderpack compile_spirv
    COMMENT "Packing shaders")
