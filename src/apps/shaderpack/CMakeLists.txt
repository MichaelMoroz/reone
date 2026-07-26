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
set(SLANG_GENERATED_DIR ${CMAKE_BINARY_DIR}/slang-glsl)

# Each entry is <slang source>:<entry point>:<stage>:<output resref>
set(SLANG_SHADERS
    "pbr_opaque_model:vertexMain:vertex:v_slang_model"
    "pbr_opaque_model:fragmentMain:fragment:f_slang_opaqmdl"
    "pbr_model:staticVertex:vertex:v_sl_mdl_static"
    "pbr_model:skinnedVertex:vertex:v_sl_mdl_skin"
    "pbr_model:danglyVertex:vertex:v_sl_mdl_dangly"
    "pbr_model:saberVertex:vertex:v_sl_mdl_saber"
    "pbr_model:opaqueFragment:fragment:f_sl_mdl_opaque")

if(SLANGC_EXECUTABLE)
    set(SLANG_OUTPUTS)
    foreach(entry ${SLANG_SHADERS})
        string(REPLACE ":" ";" parts ${entry})
        list(GET parts 0 slang_source)
        list(GET parts 1 slang_entry)
        list(GET parts 2 slang_stage)
        list(GET parts 3 slang_resref)
        set(out ${SLANG_GENERATED_DIR}/${slang_resref}.glsl)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SLANG_GENERATED_DIR}
            COMMAND ${SLANGC_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/slang/${slang_source}.slang
                -I ${CMAKE_SOURCE_DIR}/slang
                -target glsl -entry ${slang_entry} -stage ${slang_stage}
                -matrix-layout-column-major
                -o ${out}
            COMMAND ${CMAKE_COMMAND} -DFILE=${out} -P ${CMAKE_SOURCE_DIR}/cmake/SlangGLCompat.cmake
            DEPENDS ${CMAKE_SOURCE_DIR}/slang/${slang_source}.slang
                    ${CMAKE_SOURCE_DIR}/slang/uniforms.slang
                    ${CMAKE_SOURCE_DIR}/cmake/SlangGLCompat.cmake
            COMMENT "slangc ${slang_source}.slang -> ${slang_resref}.glsl")
        list(APPEND SLANG_OUTPUTS ${out})
    endforeach()
    add_custom_target(transpile_slang DEPENDS ${SLANG_OUTPUTS})
else()
    add_custom_target(transpile_slang)
endif()

add_custom_target(create_shaderpack ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SLANG_GENERATED_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/glsl ${SHADER_STAGE_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${SLANG_GENERATED_DIR} ${SHADER_STAGE_DIR}
    COMMAND shaderpack ${SHADER_STAGE_DIR} ${CMAKE_BINARY_DIR}$<$<CONFIG:Debug>:/debug>/bin
    DEPENDS shaderpack transpile_slang
    COMMENT "Packing shaders")
