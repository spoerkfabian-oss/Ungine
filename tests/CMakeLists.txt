add_executable(EngineTests CoreTests.cpp ScriptTests.cpp LifecycleTests.cpp StreamingTests.cpp ProjectTests.cpp AudioTests.cpp BlueprintTests.cpp Blueprint2Tests.cpp BlueprintTestUtil.h Test.h)
target_compile_definitions(EngineTests PRIVATE ENGINE_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(EngineTests PRIVATE Engine stb nlohmann_json::nlohmann_json)
target_compile_definitions(EngineTests PRIVATE ENGINE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
engine_set_warnings(EngineTests)
set_target_properties(EngineTests PROPERTIES FOLDER "Tests")
add_test(NAME EngineTests COMMAND EngineTests)

# Needs a Vulkan 1.3 device and a display (CI: Xvfb + Mesa lavapipe).
option(ENGINE_GPU_TESTS "Register GPU integration tests with CTest" OFF)

add_executable(EngineGpuTests GpuTests.cpp Test.h)
target_link_libraries(EngineGpuTests PRIVATE Engine Editor stb)
target_compile_definitions(EngineGpuTests PRIVATE ENGINE_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
engine_set_warnings(EngineGpuTests)
set_target_properties(EngineGpuTests PROPERTIES FOLDER "Tests")

if(ENGINE_GPU_TESTS)
    add_test(NAME EngineGpuTests COMMAND EngineGpuTests)
    add_test(NAME SandboxSmoke COMMAND Sandbox assets/models/BoxTextured.glb --frames 120
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME SandboxEditorSmoke COMMAND Sandbox assets/models/BoxTextured.glb --frames 120 --editor --lights 64
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME SandboxLightsSmoke COMMAND Sandbox assets/models/BoxTextured.glb --frames 60 --lights 1024
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME SandboxInstancesSmoke COMMAND Sandbox assets/models/BoxTextured.glb --frames 60 --instances 5000 --lights 64
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME SandboxPhysicsSmoke COMMAND Sandbox assets/models/WaterBottle.glb --frames 120 --physics 300
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME SandboxCpuCullingSmoke COMMAND Sandbox assets/models/BoxTextured.glb --frames 60 --instances 2000 --lights 64 --cpu-culling
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME SandboxPhysicsEditorSmoke COMMAND Sandbox assets/models/BoxTextured.glb --frames 60 --editor --physics 100
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # Hot reload of assets + shaders polling every frame, LOD debug view (7) on the stress grid.
    add_test(NAME SandboxHotReloadSmoke COMMAND Sandbox assets/models/BoxTextured.glb --frames 60 --hot-reload --instances 500 --debug-view 7
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    # The editor and the player on a project made from the physics template (Blueprint spawner).
    set(smoke_project "${CMAKE_BINARY_DIR}/smoke_project")
    add_test(NAME SmokeProjectSetup COMMAND ${CMAKE_COMMAND} -DTEMPLATE=${CMAKE_SOURCE_DIR}/templates/Physics
             -DDIR=${smoke_project} -DNAME=SmokeGame -P ${CMAKE_CURRENT_SOURCE_DIR}/MakeProject.cmake)
    set_tests_properties(SmokeProjectSetup PROPERTIES FIXTURES_SETUP smoke_project)
    add_test(NAME UngineEditorSmoke COMMAND UngineEditor ${smoke_project}/SmokeGame.ungineproj --frames 120)
    add_test(NAME UnginePlayerSmoke COMMAND UnginePlayer ${smoke_project}/SmokeGame.ungineproj --frames 200)
    set_tests_properties(UngineEditorSmoke UnginePlayerSmoke PROPERTIES FIXTURES_REQUIRED smoke_project RESOURCE_LOCK smoke_project)
    # Basic template: streamed music + a 3D looping source (audio through the AssetManager).
    set(smoke_basic "${CMAKE_BINARY_DIR}/smoke_basic")
    add_test(NAME SmokeBasicProjectSetup COMMAND ${CMAKE_COMMAND} -DTEMPLATE=${CMAKE_SOURCE_DIR}/templates/Basic
             -DDIR=${smoke_basic} -DNAME=BasicGame -P ${CMAKE_CURRENT_SOURCE_DIR}/MakeProject.cmake)
    set_tests_properties(SmokeBasicProjectSetup PROPERTIES FIXTURES_SETUP smoke_basic)
    add_test(NAME UnginePlayerBasicSmoke COMMAND UnginePlayer ${smoke_basic}/BasicGame.ungineproj --frames 200)
    set_tests_properties(UnginePlayerBasicSmoke PROPERTIES FIXTURES_REQUIRED smoke_basic)
    # Open Level in the background behind a custom loading screen (Basic template + start scene).
    set(smoke_switch "${CMAKE_BINARY_DIR}/smoke_switch")
    add_test(NAME SmokeLevelSwitchProjectSetup COMMAND ${CMAKE_COMMAND} -DTEMPLATE=${CMAKE_SOURCE_DIR}/templates/Basic
             -DDIR=${smoke_switch} -DNAME=SwitchGame -DLEVEL_SWITCH=ON -P ${CMAKE_CURRENT_SOURCE_DIR}/MakeProject.cmake)
    set_tests_properties(SmokeLevelSwitchProjectSetup PROPERTIES FIXTURES_SETUP smoke_switch)
    add_test(NAME UnginePlayerLevelSwitchSmoke COMMAND UnginePlayer ${smoke_switch}/SwitchGame.ungineproj --frames 200)
    # PASS_REGULAR_EXPRESSION ignores the exit code: errors (validation included) fail via FAIL_REGULAR_EXPRESSION.
    set_tests_properties(UnginePlayerLevelSwitchSmoke PROPERTIES FIXTURES_REQUIRED smoke_switch
                         PASS_REGULAR_EXPRESSION "Playing 'SwitchGame' \\([^)]*Annex\\.scene\\.json\\)"
                         FAIL_REGULAR_EXPRESSION "\\[ERROR\\]|not used")
endif()
