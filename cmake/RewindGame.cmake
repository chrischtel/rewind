# rewind_add_game(<name> SOURCES <files...>)
#
# Creates two targets from the same sources:
#   <name>       a normal executable with the game linked in (ship this)
#   <name>_hot   a DLL for `rewind_run`, which reloads it whenever it is rebuilt
#
# The sources end with REWIND_GAME(YourGameType), which expands to main() in
# the executable and to the DLL entry points in the hot build.
function(rewind_add_game name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE rewind::rewind)
    rewind_apply_flags(${name})

    add_library(${name}_hot SHARED ${ARG_SOURCES})
    target_link_libraries(${name}_hot PRIVATE rewind::rewind)
    target_compile_definitions(${name}_hot PRIVATE REWIND_GAME_DLL)
    set_target_properties(${name}_hot PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    rewind_apply_flags(${name}_hot)
endfunction()
