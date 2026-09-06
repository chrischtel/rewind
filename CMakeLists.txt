cmake_minimum_required(VERSION 3.24)

project(Rewind
    VERSION 0.1.0
    DESCRIPTION "A 2D game engine where you never lose your place while debugging."
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Every executable and DLL lands in one place so the runner, the engine DLL,
# and hot-reloadable game DLLs can always find each other.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(REWIND_BUILD_SAMPLES "Build the sample games" ON)
option(REWIND_BUILD_TESTS "Build the engine tests" ON)

include(cmake/Dependencies.cmake)
include(cmake/CompilerFlags.cmake)
include(cmake/RewindGame.cmake)

add_subdirectory(engine)
add_subdirectory(tools/runner)
add_subdirectory(tools/editor)

if(REWIND_BUILD_SAMPLES)
    add_subdirectory(samples/bounce)
    add_subdirectory(samples/pixels)
    add_subdirectory(samples/duel)
    add_subdirectory(samples/towerdefense)
endif()

if(REWIND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
