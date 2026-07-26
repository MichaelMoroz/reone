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

# SLANGC_EXECUTABLE is located by the top-level CMakeLists. Regenerating the
# layout assertions is opt-in - the generated header is committed, so only
# someone changing a uniform block needs to run this.
if(SLANGC_EXECUTABLE)
    set(UNIFORM_REFLECTION ${CMAKE_BINARY_DIR}/uniforms-reflection.json)
    add_custom_target(uniformlayout
        COMMAND ${SLANGC_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/slang/uniformreflect.slang
            -I ${CMAKE_SOURCE_DIR}/slang
            -target glsl -entry main -stage compute
            -matrix-layout-column-major
            -reflection-json ${UNIFORM_REFLECTION}
            -o ${CMAKE_BINARY_DIR}/uniforms-ignored.glsl
        COMMAND uniformgen
            ${UNIFORM_REFLECTION}
            ${REONE_INCLUDE_DIR}/reone/graphics/uniformlayout.generated.h
        DEPENDS uniformgen
            ${CMAKE_SOURCE_DIR}/slang/uniformreflect.slang
            ${CMAKE_SOURCE_DIR}/slang/uniforms.slang
        COMMENT "Regenerating uniform layout assertions from slang/uniforms.slang")
endif()
