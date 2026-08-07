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

set(GRAPHICS_INCLUDE_DIR ${REONE_INCLUDE_DIR}/reone/graphics)
set(GRAPHICS_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/libs/graphics)

set(GRAPHICS_HEADERS
    ${GRAPHICS_INCLUDE_DIR}/aabb.h
    ${GRAPHICS_INCLUDE_DIR}/animation.h
    ${GRAPHICS_INCLUDE_DIR}/barycentricutil.h
    ${GRAPHICS_INCLUDE_DIR}/camera.h
    ${GRAPHICS_INCLUDE_DIR}/camera/orthographic.h
    ${GRAPHICS_INCLUDE_DIR}/camera/perspective.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/computepipeline.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/commandbuffer.h
    ${GRAPHICS_INCLUDE_DIR}/cursor.h
    ${GRAPHICS_INCLUDE_DIR}/di/module.h
    ${GRAPHICS_INCLUDE_DIR}/di/services.h
    ${GRAPHICS_INCLUDE_DIR}/dxtutil.h
    ${GRAPHICS_INCLUDE_DIR}/font.h
    ${GRAPHICS_INCLUDE_DIR}/frustum.h
    ${GRAPHICS_INCLUDE_DIR}/rendering/gpuscene.h
    ${GRAPHICS_INCLUDE_DIR}/rendering/gpuscenecontext.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/buffer.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/image.h
    ${GRAPHICS_INCLUDE_DIR}/rendering/pbrtextures.h
    ${GRAPHICS_INCLUDE_DIR}/rendering/rayquery.h
    ${GRAPHICS_INCLUDE_DIR}/rendering/sky.h
    ${GRAPHICS_INCLUDE_DIR}/rendering/tracingpipeline.h
    ${GRAPHICS_INCLUDE_DIR}/rendering/gbuffer.h
    ${GRAPHICS_INCLUDE_DIR}/rendering/scenepipeline.h
    ${GRAPHICS_INCLUDE_DIR}/format/bwmreader.h
    ${GRAPHICS_INCLUDE_DIR}/format/curreader.h
    ${GRAPHICS_INCLUDE_DIR}/format/lipreader.h
    ${GRAPHICS_INCLUDE_DIR}/format/lipwriter.h
    ${GRAPHICS_INCLUDE_DIR}/format/mdlmdxreader.h
    ${GRAPHICS_INCLUDE_DIR}/format/tgareader.h
    ${GRAPHICS_INCLUDE_DIR}/format/tgawriter.h
    ${GRAPHICS_INCLUDE_DIR}/format/tpcreader.h
    ${GRAPHICS_INCLUDE_DIR}/format/txireader.h
    ${GRAPHICS_INCLUDE_DIR}/keyframetrack.h
    ${GRAPHICS_INCLUDE_DIR}/lipanimation.h
    ${GRAPHICS_INCLUDE_DIR}/lumautil.h
    ${GRAPHICS_INCLUDE_DIR}/material.h
    ${GRAPHICS_INCLUDE_DIR}/mesh.h
    ${GRAPHICS_INCLUDE_DIR}/meshregistry.h
    ${GRAPHICS_INCLUDE_DIR}/model.h
    ${GRAPHICS_INCLUDE_DIR}/npyutil.h
    ${GRAPHICS_INCLUDE_DIR}/modelnode.h
    ${GRAPHICS_INCLUDE_DIR}/options.h
    ${GRAPHICS_INCLUDE_DIR}/optionsregistry.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/descriptors.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/pipelinecache.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/renderer.h
    ${GRAPHICS_INCLUDE_DIR}/rendering/renderer2d.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/resources.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/rhi.h
    ${GRAPHICS_INCLUDE_DIR}/statistic.h
    ${GRAPHICS_INCLUDE_DIR}/texture.h
    ${GRAPHICS_INCLUDE_DIR}/textureregistry.h
    ${GRAPHICS_INCLUDE_DIR}/textureutil.h
    ${GRAPHICS_INCLUDE_DIR}/textutil.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/tracingstructure.h
    ${GRAPHICS_INCLUDE_DIR}/triangleutil.h
    ${GRAPHICS_INCLUDE_DIR}/types.h
    ${GRAPHICS_INCLUDE_DIR}/uniforms.h
    ${GRAPHICS_INCLUDE_DIR}/rhi/uniformring.h
    ${GRAPHICS_INCLUDE_DIR}/walkmesh.h
    ${GRAPHICS_INCLUDE_DIR}/window.h)

set(GRAPHICS_SOURCES
    ${GRAPHICS_SOURCE_DIR}/aabb.cpp
    ${GRAPHICS_SOURCE_DIR}/animation.cpp
    ${GRAPHICS_SOURCE_DIR}/camera.cpp
    ${GRAPHICS_SOURCE_DIR}/cursor.cpp
    ${GRAPHICS_SOURCE_DIR}/di/module.cpp
    ${GRAPHICS_SOURCE_DIR}/dxtutil.cpp
    ${GRAPHICS_SOURCE_DIR}/font.cpp
    ${GRAPHICS_SOURCE_DIR}/frustum.cpp
    ${GRAPHICS_SOURCE_DIR}/rendering/gpuscene.cpp
    ${GRAPHICS_SOURCE_DIR}/format/bwmreader.cpp
    ${GRAPHICS_SOURCE_DIR}/format/curreader.cpp
    ${GRAPHICS_SOURCE_DIR}/format/lipreader.cpp
    ${GRAPHICS_SOURCE_DIR}/format/lipwriter.cpp
    ${GRAPHICS_SOURCE_DIR}/format/mdlmdxreader.cpp
    ${GRAPHICS_SOURCE_DIR}/format/tgareader.cpp
    ${GRAPHICS_SOURCE_DIR}/format/tgawriter.cpp
    ${GRAPHICS_SOURCE_DIR}/format/tpcreader.cpp
    ${GRAPHICS_SOURCE_DIR}/format/txireader.cpp
    ${GRAPHICS_SOURCE_DIR}/lipanimation.cpp
    ${GRAPHICS_SOURCE_DIR}/mesh.cpp
    ${GRAPHICS_SOURCE_DIR}/meshregistry.cpp
    ${GRAPHICS_SOURCE_DIR}/model.cpp
    ${GRAPHICS_SOURCE_DIR}/npyutil.cpp
    ${GRAPHICS_SOURCE_DIR}/modelnode.cpp
    ${GRAPHICS_SOURCE_DIR}/optionsregistry.cpp
    ${GRAPHICS_SOURCE_DIR}/rendering/pbrtextures.cpp
    ${GRAPHICS_SOURCE_DIR}/rendering/rayquery.cpp
    ${GRAPHICS_SOURCE_DIR}/rendering/sky.cpp
    ${GRAPHICS_SOURCE_DIR}/rendering/tracingpipeline.cpp
    ${GRAPHICS_SOURCE_DIR}/rendering/gbuffer.cpp
    ${GRAPHICS_SOURCE_DIR}/rendering/renderer2d.cpp
    ${GRAPHICS_SOURCE_DIR}/rendering/scenepipeline.cpp
    ${GRAPHICS_SOURCE_DIR}/texture.cpp
    ${GRAPHICS_SOURCE_DIR}/textureregistry.cpp
    ${GRAPHICS_SOURCE_DIR}/textureutil.cpp
    ${GRAPHICS_SOURCE_DIR}/textutil.cpp
    ${GRAPHICS_SOURCE_DIR}/uniforms.cpp
    ${GRAPHICS_SOURCE_DIR}/walkmesh.cpp
    ${GRAPHICS_SOURCE_DIR}/window.cpp)

add_library(graphics STATIC ${GRAPHICS_HEADERS} ${GRAPHICS_SOURCES} ${CLANG_FORMAT_PATH})
set_target_properties(graphics PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}$<$<CONFIG:Debug>:/debug>/lib)
set_target_properties(graphics PROPERTIES DEBUG_POSTFIX "d")
target_precompile_headers(graphics PRIVATE ${CMAKE_SOURCE_DIR}/src/pch.h)

# The relocated RHI clients include the backend's opaque resource wrappers.
# Their implementation remains in graphicsvulkan, while these header-only
# dependencies are needed to compile the client declarations.
target_link_libraries(graphics PUBLIC
    Vulkan::Headers
    volk::volk_headers
    GPUOpen::VulkanMemoryAllocator)
target_compile_definitions(graphics PRIVATE
    REONE_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/slang"
    REONE_ASSET_SOURCE_DIR="${CMAKE_SOURCE_DIR}/assets")

if(ENABLE_FSR)
    target_link_libraries(graphics PRIVATE fsr2)
endif()

if(ENABLE_NRD)
    target_link_libraries(graphics PRIVATE NRD)
endif()

if(MSVC)
    target_link_libraries(graphics PRIVATE SDL3::SDL3)
else()
    target_link_libraries(graphics PRIVATE ${SDL3_LIBRARIES} ${SDL3_LDFLAGS})
endif()
