include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- Vulkan headers (pinned; SDK only needed at runtime for validation layers + glslc) ---
FetchContent_Declare(Vulkan-Headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        v1.4.363
    GIT_SHALLOW    TRUE
    SYSTEM)  # third-party headers: no warnings in our /W4 builds
FetchContent_MakeAvailable(Vulkan-Headers)   # provides Vulkan::Headers

# --- volk: meta-loader, no link against vulkan-1.lib ---
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG        vulkan-sdk-1.4.357.0
    GIT_SHALLOW    TRUE
    SYSTEM)  # third-party headers: no warnings in our /W4 builds
FetchContent_MakeAvailable(volk)
target_link_libraries(volk PUBLIC Vulkan::Headers)

# --- vk-bootstrap: instance/device selection boilerplate ---
set(VK_BOOTSTRAP_TEST    OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(vk_bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG        v1.4.363
    GIT_SHALLOW    TRUE
    SYSTEM)  # third-party headers: no warnings in our /W4 builds
FetchContent_MakeAvailable(vk_bootstrap)

# --- Vulkan Memory Allocator ---
FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
    SYSTEM)  # third-party headers: no warnings in our /W4 builds
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# --- GLFW ---
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
    SYSTEM)  # third-party headers: no warnings in our /W4 builds
FetchContent_MakeAvailable(glfw)

# --- GLM ---
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE
    SYSTEM)  # third-party headers: no warnings in our /W4 builds
FetchContent_MakeAvailable(glm)

# --- cgltf (glTF 2.0 parser, single header) ---
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _none) # headers only: don't run its CMakeLists
FetchContent_MakeAvailable(cgltf)
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")

# --- stb_image (no release tags: pinned commit) ---
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20
    SOURCE_SUBDIR  _none)
FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# --- meshoptimizer (engine-private: vertex cache / fetch optimization, LOD simplification) ---
FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG        v0.25
    GIT_SHALLOW    TRUE
    SYSTEM)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL           OFF CACHE BOOL "" FORCE) # our install/package holds only the tools
FetchContent_MakeAvailable(meshoptimizer)

# --- bc7enc_rdo (BC7 + BC4/BC5 block encoders / decoders; MIT or public domain, pinned commit) ---
FetchContent_Declare(bc7enc
    GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
    GIT_TAG        b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    SOURCE_SUBDIR  _none) # its CMakeLists builds a command line tool
FetchContent_MakeAvailable(bc7enc)
add_library(bc7enc STATIC "${bc7enc_SOURCE_DIR}/bc7enc.cpp" "${bc7enc_SOURCE_DIR}/rgbcx.cpp"
                          "${bc7enc_SOURCE_DIR}/bc7decomp.cpp")
target_include_directories(bc7enc SYSTEM PUBLIC "${bc7enc_SOURCE_DIR}")
set_target_properties(bc7enc PROPERTIES FOLDER "ThirdParty")

# Texture encoding and mesh simplification run on asset workers even in Debug builds: optimize
# them (GCC / Clang; MSVC Debug keeps /RTC1, which /O2 conflicts with).
foreach(target bc7enc meshoptimizer)
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -O2 -w)
    endif()
endforeach()

# --- Jolt Physics (engine-private; RTTI on: engine classes derive from Jolt interfaces) ---
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)      # keep our flags (sanitizers, MSVC runtime)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(USE_ASSERTS ON CACHE BOOL "" FORCE)             # routed to the engine log (PhysicsWorld)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.6.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build
    SYSTEM)  # third-party headers: no warnings in our /W4 builds
FetchContent_MakeAvailable(JoltPhysics)

# --- nlohmann/json: scene files, editor snapshots (engine-private) ---
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    SYSTEM)  # third-party headers: no warnings in our /W4 builds
FetchContent_MakeAvailable(nlohmann_json)

# --- Dear ImGui (docking branch) + GLFW/Vulkan backends: editor only ---
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9-docking
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _none) # no CMake project upstream
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp) # InputText with std::string
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends
                           ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_definitions(imgui PUBLIC
    IMGUI_IMPL_VULKAN_USE_VOLK        # backend calls through volk's function pointers
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    GLFW_INCLUDE_NONE)
target_link_libraries(imgui PUBLIC glfw volk)
set_target_properties(imgui PROPERTIES FOLDER "ThirdParty")

# --- ImGuizmo (no release tags for current ImGui: pinned commit) ---
FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG        18cef5e031d8c6973d80284c67f60549fafd78c1
    SOURCE_SUBDIR  _none)
FetchContent_MakeAvailable(imguizmo)
add_library(imguizmo STATIC ${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp)
target_include_directories(imguizmo SYSTEM PUBLIC ${imguizmo_SOURCE_DIR}/src)
target_link_libraries(imguizmo PUBLIC imgui)
set_target_properties(imguizmo PROPERTIES FOLDER "ThirdParty")
