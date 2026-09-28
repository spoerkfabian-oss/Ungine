add_executable(Sandbox src/main.cpp)
target_link_libraries(Sandbox PRIVATE Engine)
engine_set_warnings(Sandbox)
set_target_properties(Sandbox PROPERTIES
    FOLDER "Apps"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

