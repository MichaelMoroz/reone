# Copyright (c) 2026 The reone project contributors
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

set(SKYBAKE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/apps/skybake)

set(SKYBAKE_HEADERS
    ${SKYBAKE_SOURCE_DIR}/image.h
    ${SKYBAKE_SOURCE_DIR}/skybaker.h)

set(SKYBAKE_SOURCES
    ${SKYBAKE_SOURCE_DIR}/image.cpp
    ${SKYBAKE_SOURCE_DIR}/main.cpp
    ${SKYBAKE_SOURCE_DIR}/skybaker.cpp)

add_executable(skybake ${SKYBAKE_SOURCES} ${SKYBAKE_HEADERS} ${CLANG_FORMAT_PATH})
set_target_properties(skybake PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${REONE_BIN_DIR})
target_precompile_headers(skybake PRIVATE ${CMAKE_SOURCE_DIR}/src/pch.h)
target_link_libraries(skybake PRIVATE resource graphics system ${Boost_PROGRAM_OPTIONS_LIBRARY})
