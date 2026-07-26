# Copyright (c) 2026 The reone project contributors

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

set(UNIFORMGEN_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/apps/uniformgen)

add_executable(uniformgen ${UNIFORMGEN_SOURCE_DIR}/main.cpp ${CLANG_FORMAT_PATH})
set_target_properties(uniformgen PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}$<$<CONFIG:Debug>:/debug>/bin)
target_link_libraries(uniformgen PRIVATE ${Boost_PROGRAM_OPTIONS_LIBRARY} ${Boost_JSON_LIBRARY})

# Regenerating the layout assertions needs slangc, which most builds have no
# reason to require - the generated header is committed. Opt in when a uniform
# block changes.
find_program(SLANGC_EXECUTABLE slangc
    HINTS ${CMAKE_PREFIX_PATH} ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shader-slang
    DOC "Slang compiler, used to regenerate uniform layout assertions")

if(SLANGC_EXECUTABLE)
    set(UNIFORM_REFLECTION ${CMAKE_BINARY_DIR}/uniforms-reflection.json)
    add_custom_target(uniformlayout
        COMMAND ${SLANGC_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/slang/uniforms.slang
            -target glsl -entry main -stage compute
            -matrix-layout-column-major
            -reflection-json ${UNIFORM_REFLECTION}
            -o ${CMAKE_BINARY_DIR}/uniforms-ignored.glsl
        COMMAND uniformgen
            ${UNIFORM_REFLECTION}
            ${REONE_INCLUDE_DIR}/reone/graphics/uniformlayout.generated.h
        DEPENDS uniformgen ${CMAKE_SOURCE_DIR}/slang/uniforms.slang
        COMMENT "Regenerating uniform layout assertions from slang/uniforms.slang")
endif()
