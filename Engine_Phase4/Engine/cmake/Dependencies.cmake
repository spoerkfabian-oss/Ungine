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
