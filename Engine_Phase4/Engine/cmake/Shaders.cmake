# GLSL -> SPIR-V at build time. glslc ships with the Vulkan SDK.
find_program(GLSLC_EXECUTABLE glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)

set(ENGINE_SHADER_OUTPUT_DIR  "${CMAKE_BINARY_DIR}/shaders")
set(ENGINE_SHADER_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/engine/shaders/include")

# engine_add_shaders(<target> SOURCES a.vert b.frag ...)
# Output: ${ENGINE_SHADER_OUTPUT_DIR}/<file>.spv. #include changes trigger rebuilds via depfiles.
function(engine_add_shaders target)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    set(spv_files)
    foreach(src IN LISTS ARG_SOURCES)
        get_filename_component(abs "${src}" ABSOLUTE)
        get_filename_component(name "${src}" NAME)
        set(spv "${ENGINE_SHADER_OUTPUT_DIR}/${name}.spv")
        add_custom_command(
            OUTPUT  "${spv}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ENGINE_SHADER_OUTPUT_DIR}"
            COMMAND ${GLSLC_EXECUTABLE} --target-env=vulkan1.3 -Werror
                    "$<IF:$<CONFIG:Debug>,-g;-O0,-O>"
                    -I "${ENGINE_SHADER_INCLUDE_DIR}"
                    -MD -MF "${spv}.d" -o "${spv}" "${abs}"
            DEPENDS "${abs}"
            DEPFILE "${spv}.d"
            COMMENT "GLSL -> SPIR-V: ${name}"
            VERBATIM COMMAND_EXPAND_LISTS)
        list(APPEND spv_files "${spv}")
    endforeach()
    add_custom_target(${target}_Shaders DEPENDS ${spv_files} SOURCES ${ARG_SOURCES})
    set_target_properties(${target}_Shaders PROPERTIES FOLDER "Shaders")
    add_dependencies(${target} ${target}_Shaders)
endfunction()
