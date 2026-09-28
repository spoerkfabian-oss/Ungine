add_executable(EngineTests CoreTests.cpp Test.h)
target_link_libraries(EngineTests PRIVATE Engine)
engine_set_warnings(EngineTests)
set_target_properties(EngineTests PROPERTIES FOLDER "Tests")
add_test(NAME EngineTests COMMAND EngineTests)

# Needs a Vulkan 1.3 device and a display (CI: Xvfb + Mesa lavapipe).
option(ENGINE_GPU_TESTS "Register GPU integration tests with CTest" OFF)

add_executable(EngineGpuTests GpuTests.cpp Test.h)
target_link_libraries(EngineGpuTests PRIVATE Engine Editor)
target_compile_definitions(EngineGpuTests PRIVATE ENGINE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
engine_set_warnings(EngineGpuTests)
set_target_properties(EngineGpuTests PROPERTIES FOLDER "Tests")

if(ENGINE_GPU_TESTS)
    add_test(NAME EngineGpuTests COMMAND EngineGpuTests)
    add_test(NAME SandboxSmoke COMMAND Sandbox assets/models/BoxTextured.glb --frames 120
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME SandboxEditorSmoke COMMAND Sandbox assets/models/BoxTextured.glb --frames 120 --editor
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
