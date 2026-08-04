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

set(ENGINE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/apps/engine)

set(ENGINE_HEADERS
    ${ENGINE_SOURCE_DIR}/console.h
    ${ENGINE_SOURCE_DIR}/engine.h
    ${ENGINE_SOURCE_DIR}/options.h
    ${ENGINE_SOURCE_DIR}/optionsparser.h
    ${ENGINE_SOURCE_DIR}/profiler.h
    ${ENGINE_SOURCE_DIR}/editor.h)

set(ENGINE_HEADERS
    ${CMAKE_SOURCE_DIR}/src/apps/highperfgfx.cpp
    ${ENGINE_SOURCE_DIR}/console.cpp
    ${ENGINE_SOURCE_DIR}/engine.cpp
    ${ENGINE_SOURCE_DIR}/main.cpp
    ${ENGINE_SOURCE_DIR}/optionsparser.cpp
    ${ENGINE_SOURCE_DIR}/profiler.cpp
    ${ENGINE_SOURCE_DIR}/editor.cpp)

if(WIN32)
    list(APPEND ENGINE_HEADERS ${CMAKE_SOURCE_DIR}/assets/reone.rc)
endif()

add_executable(engine ${ENGINE_HEADERS} ${ENGINE_HEADERS} ${CLANG_FORMAT_PATH})
set_target_properties(engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}$<$<CONFIG:Debug>:/debug>/bin)
target_include_directories(engine PRIVATE ${CMAKE_SOURCE_DIR}/extern)
target_precompile_headers(engine PRIVATE ${CMAKE_SOURCE_DIR}/src/pch.h)
target_link_libraries(engine PRIVATE game ${Boost_PROGRAM_OPTIONS_LIBRARY} imgui::imgui)

target_link_libraries(engine PRIVATE graphicsvulkan)

# Slang loads helper DLLs (the compiler and back ends) beside slang.dll.
file(GLOB SLANG_RUNTIME_DLLS
    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/slang*.dll
    ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/gfx.dll)
foreach(dll ${SLANG_RUNTIME_DLLS})
    add_custom_command(TARGET engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dll} $<TARGET_FILE_DIR:engine>)
endforeach()
add_custom_command(TARGET engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/slang $<TARGET_FILE_DIR:engine>/slang)

list(APPEND InstallTargets engine)
