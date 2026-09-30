# UngineEditor (editor + project browser) and UnginePlayer (runs a project), installed with CPack.
# Both land in the same output directory: Build & Run finds the player next to the editor.
add_executable(UngineEditor editor/main.cpp)
target_link_libraries(UngineEditor PRIVATE Engine Editor)

add_executable(UnginePlayer player/main.cpp)
target_link_libraries(UnginePlayer PRIVATE Engine)

foreach(app UngineEditor UnginePlayer)
    engine_set_warnings(${app})
    set_target_properties(${app} PROPERTIES FOLDER "Apps" VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        target_sources(${app} PRIVATE "${CMAKE_SOURCE_DIR}/resources/windows/ungine.rc") # exe icon
        if(MSVC) # no console window, still entering through main()
            target_link_options(${app} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
        endif()
    endif()
endforeach()

# --- Install layout (relative, so the tools find their data next to the executables) ---
#   bin/UngineEditor, bin/UnginePlayer, bin/shaders/*.spv, bin/templates/, bin/samples/, bin/resources/
install(TARGETS UngineEditor UnginePlayer RUNTIME DESTINATION bin)
install(DIRECTORY "${ENGINE_SHADER_OUTPUT_DIR}/" DESTINATION bin/shaders FILES_MATCHING PATTERN "*.spv")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/templates/" DESTINATION bin/templates)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/models" "${CMAKE_SOURCE_DIR}/assets/scripts" "${CMAKE_SOURCE_DIR}/assets/sounds"
        DESTINATION bin/samples)
install(FILES "${CMAKE_SOURCE_DIR}/resources/ungine.png" "${CMAKE_SOURCE_DIR}/resources/ungine.ico" DESTINATION bin/resources)
if(UNIX AND NOT APPLE)
    install(FILES "${CMAKE_SOURCE_DIR}/resources/linux/ungine-editor.desktop.in"
                  "${CMAKE_SOURCE_DIR}/resources/linux/ungine-mime.xml"
            DESTINATION bin/resources)
    install(PROGRAMS "${CMAKE_SOURCE_DIR}/resources/linux/install-desktop-integration.sh" DESTINATION bin)
endif()
