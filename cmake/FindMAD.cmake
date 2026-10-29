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

find_path(MAD_INCLUDE_DIR mad.h DOC "MAD include directory")

# Release and Debug are located separately and bound per configuration. A
# single find_library answers with whichever of vcpkg's two library paths
# comes first, and on MSVC that has been the debug one - so a Release build
# linked a debug-CRT library, mixed two heaps in one process, and corrupted
# the heap intermittently rather than failing outright.
# The two roots are derived from the include directory so this keeps working
# with any prefix that uses the lib/ + debug/lib/ split, without naming a
# package manager here.
get_filename_component(MAD_PREFIX "${MAD_INCLUDE_DIR}" DIRECTORY)
find_library(MAD_LIBRARY_RELEASE NAMES mad DOC "MAD library (release)"
    PATHS ${MAD_PREFIX}/lib NO_DEFAULT_PATH)
find_library(MAD_LIBRARY_DEBUG NAMES mad DOC "MAD library (debug)"
    PATHS ${MAD_PREFIX}/debug/lib NO_DEFAULT_PATH)
if(NOT MAD_LIBRARY_RELEASE AND NOT MAD_LIBRARY_DEBUG)
    find_library(MAD_LIBRARY_RELEASE NAMES mad DOC "MAD library (release)")
endif()
if(NOT MAD_LIBRARY_DEBUG)
    set(MAD_LIBRARY_DEBUG ${MAD_LIBRARY_RELEASE})
endif()
if(NOT MAD_LIBRARY_RELEASE)
    set(MAD_LIBRARY_RELEASE ${MAD_LIBRARY_DEBUG})
endif()
if(MAD_LIBRARY_RELEASE)
    set(MAD_LIBRARY
        "$<IF:$<CONFIG:Debug>,${MAD_LIBRARY_DEBUG},${MAD_LIBRARY_RELEASE}>"
        CACHE STRING "MAD library, selected per configuration" FORCE)
endif()

if(MAD_INCLUDE_DIR AND MAD_LIBRARY)
    set(MAD_FOUND 1)
    set(MAD_LIBRARIES ${MAD_LIBRARY})
    set(MAD_INCLUDE_DIRS ${MAD_INCLUDE_DIR})
else()
    set(MAD_FOUND 0)
    set(MAD_LIBRARIES)
    set(MAD_INCLUDE_DIRS)
endif()

if(NOT MAD_FOUND)
    set(MAD_NOT_FOUND_MESSAGE "MAD library not found. Set MAD_INCLUDE_DIR and MAD_LIBRARY manually.")
    if(MAD_FIND_REQUIRED)
        message(FATAL_ERROR "${MAD_NOT_FOUND_MESSAGE}")
    endif()
else()
    message("MAD library found")
endif()
