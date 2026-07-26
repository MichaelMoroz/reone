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

# Shaders are staged into one flat directory before packing: the hand-written
# GLSL, plus whatever slangc transpiles from slang/. Transpiled output stays in
# the build tree - without slangc those resrefs are simply absent, and the engine
# keeps using the hand-written equivalents.
set(SHADER_STAGE_DIR ${CMAKE_BINARY_DIR}/shaders)

# One SPIR-V module per source file, carrying every entry point that file
# declares. Binding is by number, so stages of a program need no agreement on
# names - which is what the transpiled-GLSL path could not deliver. Emitted
# beside the executable; the shader pack still carries the legacy GLSL.
set(SPIRV_OUT_DIR ${CMAKE_BINARY_DIR}$<$<CONFIG:Debug>:/debug>/bin/spirv)

# <source>:<entry>[+<entry>...]
set(SPIRV_MODULES
    "pbr_model:staticVertex+skinnedVertex+danglyVertex+saberVertex+opaqueFragment"
    "grass:grassVertex+pbrFragment+retroFragment"
    "walkmesh:walkmeshVertex+pbrFragment+retroFragment")

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
                ${entry_args} -o ${out}
            DEPENDS ${CMAKE_SOURCE_DIR}/slang/${spv_source}.slang
                    ${CMAKE_SOURCE_DIR}/slang/uniforms.slang
            COMMENT "slangc ${spv_source}.slang -> ${spv_source}.spv")
        list(APPEND SPIRV_OUTPUTS ${out})
    endforeach()
    add_custom_target(transpile_spirv ALL DEPENDS ${SPIRV_OUTPUTS})
else()
    add_custom_target(transpile_spirv)
endif()

add_custom_target(create_shaderpack ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/glsl ${SHADER_STAGE_DIR}
    COMMAND shaderpack ${SHADER_STAGE_DIR} ${CMAKE_BINARY_DIR}$<$<CONFIG:Debug>:/debug>/bin
    DEPENDS shaderpack transpile_spirv
    COMMENT "Packing shaders")
