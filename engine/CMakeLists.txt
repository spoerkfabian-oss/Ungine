add_library(Engine STATIC
    src/Assets/AssetManager.cpp
    src/Assets/GltfLoader.cpp
    src/Assets/Model.cpp
    src/Assets/Primitives.cpp
    src/Core/Application.cpp
    src/Core/Input.cpp
    src/Core/ThreadPool.cpp
    src/Core/Window.cpp
    src/Events/EventBus.cpp
    src/Renderer/Environment.cpp
    src/Renderer/Renderer.cpp
    src/Renderer/SceneRenderer.cpp
    src/Renderer/ShadowCascades.cpp
    src/Renderer/Vulkan/Bindless.cpp
    src/Renderer/Vulkan/Buffer.cpp
    src/Renderer/Vulkan/Image.cpp
    src/Renderer/Vulkan/Pipeline.cpp
    src/Renderer/Vulkan/Swapchain.cpp
    src/Renderer/Vulkan/Upload.cpp
    src/Renderer/Vulkan/VkUtils.cpp
    src/Renderer/Vulkan/VulkanContext.cpp
    src/Renderer/Vulkan/VmaImpl.cpp
    src/Scene/Camera.cpp
    src/Scene/Frustum.cpp
    src/Scene/Scene.cpp
    src/ThirdParty/ThirdPartyImpl.cpp)

target_include_directories(Engine PUBLIC include PRIVATE src)

target_link_libraries(Engine
    PUBLIC  Vulkan::Headers volk vk-bootstrap::vk-bootstrap
            GPUOpen::VulkanMemoryAllocator glfw glm::glm
    PRIVATE cgltf stb)

target_compile_definitions(Engine PUBLIC
    GLFW_INCLUDE_NONE
    ENGINE_SHADER_DIR="${ENGINE_SHADER_OUTPUT_DIR}"
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    $<$<PLATFORM_ID:Windows>:NOMINMAX WIN32_LEAN_AND_MEAN>
    $<$<CONFIG:Debug>:ENGINE_DEBUG>)

engine_set_warnings(Engine)
set_target_properties(Engine PROPERTIES FOLDER "Engine")

# Shared GLSL includes (bindless.glsl, ...) show up in the IDE
set(ENGINE_SHADER_INCLUDES
    shaders/include/bindless.glsl
    shaders/include/exposure.glsl
    shaders/include/frame.glsl
    shaders/include/ibl_common.glsl
    shaders/include/mesh_common.glsl
    shaders/include/pbr.glsl
    shaders/include/shadow.glsl
    shaders/include/sky.glsl
    shaders/include/surface.glsl)
target_sources(Engine PRIVATE ${ENGINE_SHADER_INCLUDES})
set_source_files_properties(${ENGINE_SHADER_INCLUDES} PROPERTIES HEADER_FILE_ONLY TRUE)

engine_add_shaders(Engine SOURCES
    shaders/bloom_downsample.comp
    shaders/bloom_upsample.comp
    shaders/depth_normal.frag
    shaders/exposure_average.comp
    shaders/fullscreen.vert
    shaders/gtao.comp
    shaders/gtao_denoise.comp
    shaders/ibl_brdf.comp
    shaders/ibl_irradiance.comp
    shaders/ibl_prefilter.comp
    shaders/ibl_sky.comp
    shaders/luminance_histogram.comp
    shaders/mesh.frag
    shaders/mesh.vert
    shaders/shadow.vert
    shaders/shadow_mask.frag
    shaders/sky.frag
    shaders/tonemap.frag)
