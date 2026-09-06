# The engine is a shared library on purpose: game code is built as a separate
# DLL that can be swapped at runtime, and it must share this one copy of SDL,
# ImGui, and the engine itself rather than carrying its own.

# ---- shaders ----------------------------------------------------------------
# GLSL is compiled to SPIR-V at build time when glslc (Vulkan SDK) is around
# and embedded into the binary. Without it the engine still runs on Windows:
# the HLSL twin of each shader is compiled at runtime for Direct3D 12.
set(REWIND_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(REWIND_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${REWIND_GENERATED_DIR})
set(REWIND_SHADER_HEADER ${REWIND_GENERATED_DIR}/rewind_shaders_spv.h)

find_program(REWIND_GLSLC glslc HINTS $ENV{VULKAN_SDK}/Bin $ENV{VULKAN_SDK}/bin)
if(REWIND_GLSLC)
    message(STATUS "Rewind: SPIR-V shaders via ${REWIND_GLSLC}")
    set(spv_outputs)
    set(spv_names)
    foreach(shader sprite.vert sprite.frag)
        string(REPLACE "." "_" symbol ${shader})
        set(out ${REWIND_GENERATED_DIR}/${shader}.spv)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${REWIND_GLSLC} -O ${REWIND_SHADER_DIR}/${shader} -o ${out}
            DEPENDS ${REWIND_SHADER_DIR}/${shader}
            COMMENT "glslc ${shader}"
            VERBATIM)
        list(APPEND spv_outputs ${out})
        list(APPEND spv_names ${symbol}_spv)
    endforeach()
    add_custom_command(
        OUTPUT ${REWIND_SHADER_HEADER}
        COMMAND ${CMAKE_COMMAND}
            -DOUTPUT=${REWIND_SHADER_HEADER}
            "-DINPUTS=${spv_outputs}"
            "-DNAMES=${spv_names}"
            -DDEFINE=REWIND_HAVE_SPIRV
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake
        DEPENDS ${spv_outputs} ${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake
        COMMENT "embedding SPIR-V shaders"
        VERBATIM)
else()
    if(WIN32)
        message(STATUS "Rewind: glslc not found, no SPIR-V (Direct3D 12 only)")
    else()
        message(WARNING "Rewind: glslc not found. The engine builds, but rendering needs SPIR-V on this platform; install the Vulkan SDK or the glslc package.")
    endif()
    file(WRITE ${REWIND_SHADER_HEADER} "#pragma once\n#define REWIND_HAVE_SPIRV 0\n")
endif()

add_library(rewind SHARED
    src/app.cpp
    src/aseprite.cpp
    src/camera.cpp
    src/asset_db.cpp
    src/audio.cpp
    src/cooked.cpp
    src/curve.cpp
    src/debug_font.cpp
    src/file_watcher.cpp
    src/font.cpp
    src/game_module.cpp
    src/graph.cpp
    src/gizmo.cpp
    src/inspector.cpp
    src/log.cpp
    src/math.cpp
    src/mesh.cpp
    src/net_loopback.cpp
    src/net_session.cpp
    src/net_udp.cpp
    src/overlay.cpp
    src/paths.cpp
    src/png.cpp
    src/profiler.cpp
    src/project.cpp
    src/reflect.cpp
    src/renderer2d.cpp
    src/scene.cpp
    src/shader_cross.cpp
    src/sprite.cpp
    src/stb_impl.cpp
    src/text_format.cpp
    src/tilemap.cpp
    src/timeline.cpp
    ${REWIND_SHADER_HEADER}
    ${REWIND_IMGUI_SOURCES})

add_library(rewind::rewind ALIAS rewind)

target_include_directories(rewind
    PUBLIC include ${REWIND_IMGUI_INCLUDE_DIRS}
    PRIVATE ${REWIND_STB_INCLUDE_DIR} ${REWIND_GENERATED_DIR})
target_link_libraries(rewind PRIVATE SDL3::SDL3 spirv-cross-core spirv-cross-glsl spirv-cross-hlsl)
target_compile_features(rewind PUBLIC cxx_std_20)

if(REWIND_GLSLC)
    # Lets the engine recompile GLSL materials live when their source changes.
    target_compile_definitions(rewind PRIVATE "REWIND_GLSLC_PATH=\"${REWIND_GLSLC}\"")
endif()

if(WIN32)
    target_link_libraries(rewind PRIVATE d3dcompiler ws2_32)
else()
    target_link_libraries(rewind PRIVATE ${CMAKE_DL_LIBS})
endif()

set_target_properties(rewind PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

rewind_apply_flags(rewind)
