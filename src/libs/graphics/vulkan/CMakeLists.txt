# Copyright (c) 2020-2026 The reone project contributors

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

set(VULKAN_INCLUDE_DIR ${REONE_INCLUDE_DIR}/reone/graphics/vulkan)
set(VULKAN_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/libs/graphics/vulkan)

set(VULKAN_HEADERS
    ${VULKAN_INCLUDE_DIR}/buffer.h
    ${VULKAN_INCLUDE_DIR}/descriptorwrites.h
    ${VULKAN_INCLUDE_DIR}/descriptors.h
    ${VULKAN_INCLUDE_DIR}/debugscope.h
    ${VULKAN_INCLUDE_DIR}/device.h
    ${VULKAN_INCLUDE_DIR}/gbuffer.h
    ${VULKAN_INCLUDE_DIR}/gpuscene.h
    ${VULKAN_INCLUDE_DIR}/image.h
    ${VULKAN_INCLUDE_DIR}/mesh.h
    ${VULKAN_INCLUDE_DIR}/pbrtextures.h
    ${VULKAN_INCLUDE_DIR}/pipeline.h
    ${VULKAN_INCLUDE_DIR}/pipelinecache.h
    ${VULKAN_INCLUDE_DIR}/renderpass.h
    ${VULKAN_INCLUDE_DIR}/renderer.h
    ${VULKAN_INCLUDE_DIR}/renderer2d.h
    ${VULKAN_INCLUDE_DIR}/rayquery.h
    ${VULKAN_INCLUDE_DIR}/resources.h
    ${VULKAN_INCLUDE_DIR}/samplers.h
    ${VULKAN_INCLUDE_DIR}/scenepipeline.h
    ${VULKAN_INCLUDE_DIR}/shadercompiler.h
    ${VULKAN_INCLUDE_DIR}/swapchain.h
    ${VULKAN_INCLUDE_DIR}/uniformring.h)

set(VULKAN_SOURCES
    ${VULKAN_SOURCE_DIR}/buffer.cpp
    ${VULKAN_SOURCE_DIR}/descriptorwrites.cpp
    ${VULKAN_SOURCE_DIR}/descriptors.cpp
    ${VULKAN_SOURCE_DIR}/device.cpp
    ${VULKAN_SOURCE_DIR}/gbuffer.cpp
    ${VULKAN_SOURCE_DIR}/gpuscene.cpp
    ${VULKAN_SOURCE_DIR}/image.cpp
    ${VULKAN_SOURCE_DIR}/mesh.cpp
    ${VULKAN_SOURCE_DIR}/pbrtextures.cpp
    ${VULKAN_SOURCE_DIR}/pipeline.cpp
    ${VULKAN_SOURCE_DIR}/pipelinecache.cpp
    ${VULKAN_SOURCE_DIR}/renderpass.cpp
    ${VULKAN_SOURCE_DIR}/renderer.cpp
    ${VULKAN_SOURCE_DIR}/renderer2d.cpp
    ${VULKAN_SOURCE_DIR}/rayquery.cpp
    ${VULKAN_SOURCE_DIR}/resources.cpp
    ${VULKAN_SOURCE_DIR}/samplers.cpp
    ${VULKAN_SOURCE_DIR}/scenepipeline.cpp
    ${VULKAN_SOURCE_DIR}/shadercompiler.cpp
    ${VULKAN_SOURCE_DIR}/swapchain.cpp
    ${VULKAN_SOURCE_DIR}/uniformring.cpp
    ${VULKAN_SOURCE_DIR}/volk.cpp
    ${VULKAN_SOURCE_DIR}/vma.cpp)

if(ENABLE_FSR)
    list(APPEND VULKAN_HEADERS ${VULKAN_INCLUDE_DIR}/fsrupscaler.h)
    list(APPEND VULKAN_SOURCES ${VULKAN_SOURCE_DIR}/fsrupscaler.cpp)
endif()

if(ENABLE_NRD)
    list(APPEND VULKAN_HEADERS ${VULKAN_INCLUDE_DIR}/nrddenoiser.h)
    list(APPEND VULKAN_SOURCES ${VULKAN_SOURCE_DIR}/nrddenoiser.cpp)
endif()

add_library(graphicsvulkan STATIC ${VULKAN_HEADERS} ${VULKAN_SOURCES} ${CLANG_FORMAT_PATH})
set_target_properties(graphicsvulkan PROPERTIES DEBUG_POSTFIX "d")
target_precompile_headers(graphicsvulkan PRIVATE ${CMAKE_SOURCE_DIR}/src/pch.h)

# VK_NO_PROTOTYPES keeps the headers from declaring the entry points that volk
# defines as function pointers; without it the two collide at link time.
target_compile_definitions(graphicsvulkan PUBLIC
    VK_NO_PROTOTYPES
    # ImGui's packaged Vulkan backend uses loader prototypes. Namespacing
    # volk keeps its function-pointer variables from satisfying those function
    # symbols with data addresses.
    VOLK_NAMESPACE
    # volk owns the entry points, so VMA must not expect linked prototypes.
    # It has to fetch its own through the two getters it is handed at creation,
    # which is what the dynamic path does; with both of these off it calls
    # through null pointers and dies inside vmaCreateAllocator.
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
target_compile_definitions(graphicsvulkan PUBLIC
    REONE_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/slang")

if(ENABLE_FSR)
    target_link_libraries(graphicsvulkan PUBLIC fsr2)
endif()

if(ENABLE_NRD)
    target_link_libraries(graphicsvulkan PUBLIC NRD)
endif()

target_link_libraries(graphicsvulkan PUBLIC
    graphics
    system
    Vulkan::Headers
    volk::volk_headers
    GPUOpen::VulkanMemoryAllocator
    vk-bootstrap::vk-bootstrap
    slang::slang)

if(MSVC)
    target_link_libraries(graphicsvulkan PUBLIC SDL3::SDL3)
else()
    target_link_libraries(graphicsvulkan PUBLIC ${SDL3_LIBRARIES})
endif()
