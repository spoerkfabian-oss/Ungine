cmake_minimum_required(VERSION 3.25)
project(Engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(NOT MSVC)
    message(WARNING "Primary target is MSVC; other compilers are best-effort.")
endif()

# Warnings only for our own targets, never for third-party code
function(engine_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /MP)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

include(cmake/Dependencies.cmake)
include(cmake/Shaders.cmake)

add_subdirectory(engine)
add_subdirectory(editor)
add_subdirectory(sandbox)

option(ENGINE_BUILD_TESTS "Build the test executables" ${PROJECT_IS_TOP_LEVEL})
if(ENGINE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Sandbox)
