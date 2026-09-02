# rewind_add_game(<name> SOURCES <files...> [SHADERS <glsl files...>])
#
# Creates two targets from the same sources:
#   <name>       a normal executable with the game linked in (ship this)
#   <name>_hot   a DLL for `rewind_run`, which reloads it whenever it is rebuilt
#
# The sources end with REWIND_GAME(YourGameType), which expands to main() in
# the executable and to the DLL entry points in the hot build.
#
# SHADERS are GLSL files compiled to SPIR-V into <exe dir>/shaders/<file>.spv
# when glslc is available. The engine loads them by that path (relative paths
# are resolved against the executable's directory) and, when the GLSL source
# changes at runtime, recompiles them live with the same glslc.
function(rewind_add_game name)
    cmake_parse_arguments(ARG "" "" "SOURCES;SHADERS" ${ARGN})

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE rewind::rewind)
    rewind_apply_flags(${name})

    add_library(${name}_hot SHARED ${ARG_SOURCES})
    target_link_libraries(${name}_hot PRIVATE rewind::rewind)
    target_compile_definitions(${name}_hot PRIVATE REWIND_GAME_DLL)
    set_target_properties(${name}_hot PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    rewind_apply_flags(${name}_hot)

    if(ARG_SHADERS)
        rewind_add_shaders(${name}_shaders ${ARG_SHADERS})
        add_dependencies(${name} ${name}_shaders)
        add_dependencies(${name}_hot ${name}_shaders)
    endif()
endfunction()

# rewind_add_shaders(<target> <glsl files...>)
#
# Compiles each GLSL file to <exe dir>/shaders/<filename>.spv and writes a
# <filename>.spv.src sidecar holding the absolute source path, which is how
# the engine finds the source for live recompiles. The executable directory
# is <build>/bin/<Config> for multi-config generators and <build>/bin otherwise.
function(rewind_add_shaders target)
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(multi_config)
        set(out_dir "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/shaders")
    else()
        set(out_dir "${CMAKE_BINARY_DIR}/bin/shaders")
    endif()

    set(outputs)
    foreach(shader ${ARGN})
        get_filename_component(abs ${shader} ABSOLUTE)
        get_filename_component(file ${shader} NAME)
        set(spv "${out_dir}/${file}.spv")
        file(GENERATE OUTPUT "${spv}.src" CONTENT "${abs}")
        if(REWIND_GLSLC)
            add_custom_command(
                OUTPUT ${spv}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
                COMMAND ${REWIND_GLSLC} -O ${abs} -o ${spv}
                DEPENDS ${abs}
                COMMENT "glslc ${file}"
                VERBATIM)
            list(APPEND outputs ${spv})
        else()
            message(STATUS "Rewind: glslc not found, ${file} will not be compiled (material unavailable)")
        endif()
    endforeach()
    add_custom_target(${target} DEPENDS ${outputs})
endfunction()
