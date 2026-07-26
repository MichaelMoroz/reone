# Rewrites Vulkan-flavoured builtins in Slang's GLSL output to their OpenGL
# spellings.
#
# Slang's GLSL target assumes Vulkan semantics and has no OpenGL flavour, so
# SV_VertexID comes out as gl_VertexIndex, which OpenGL does not declare. This is
# a shim for the transitional period while the OpenGL backend still exists; it
# disappears with that backend, when the same Slang sources are compiled straight
# to SPIR-V and no rewriting is needed.
#
# The two indices differ in one respect: in Vulkan they include the draw's vertex
# and instance offsets, whereas in OpenGL they do not. The engine issues no
# base-vertex or base-instance draws, so the two agree here.
#
# Usage: cmake -DFILE=<path> -P SlangGLCompat.cmake

if(NOT DEFINED FILE)
    message(FATAL_ERROR "SlangGLCompat: FILE is required")
endif()

file(READ ${FILE} contents)
string(REPLACE "gl_VertexIndex" "gl_VertexID" contents "${contents}")
string(REPLACE "gl_InstanceIndex" "gl_InstanceID" contents "${contents}")
file(WRITE ${FILE} "${contents}")
