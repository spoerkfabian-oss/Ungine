add_library(Engine STATIC
    src/Assets/AssetManager.cpp
    src/Audio/AudioEngine.cpp
    src/Audio/AudioSystem.cpp
    src/Audio/Sound.cpp
    src/Assets/GltfLoader.cpp
    src/Assets/Model.cpp
    src/Assets/Primitives.cpp
    src/Assets/TextureCooker.cpp
    src/Assets/MeshOptimizer.cpp
    src/Core/Application.cpp
    src/Core/Input.cpp
    src/Core/Platform.cpp
    src/Core/Project.cpp
    src/Core/ThreadPool.cpp
    src/Core/Window.cpp
    src/Events/EventBus.cpp
    src/Renderer/Environment.cpp
    src/Renderer/GeometryPool.cpp
    src/Renderer/GpuCulling.cpp
    src/Renderer/GpuProfiler.cpp
    src/Renderer/GpuScene.cpp
    src/Renderer/RangeAllocator.cpp
    src/Renderer/Renderer.cpp
    src/Renderer/SceneRenderer.cpp
    src/Renderer/TextOverlay.cpp
    src/Renderer/ShaderReload.cpp
    src/Renderer/ShadowAtlas.cpp
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
    src/Scene/AabbTree.cpp
    src/Scene/Scene.cpp
    src/Scene/SceneSerializer.cpp
    src/Scene/Prefab.cpp
    src/Scene/SpatialIndex.cpp
    src/Physics/PhysicsWorld.cpp
    src/Script/ScriptGraph.cpp
    src/Script/ScriptNodes.cpp
    src/Script/ScriptValue.cpp
    src/Script/ScriptExpand.cpp
    src/Script/ScriptRegistry.cpp
    src/Script/ScriptSystem.cpp
    src/ThirdParty/ThirdPartyImpl.cpp)

target_include_directories(Engine PUBLIC include PRIVATE src)

target_link_libraries(Engine
    PUBLIC  Vulkan::Headers volk vk-bootstrap::vk-bootstrap
            GPUOpen::VulkanMemoryAllocator glfw glm::glm
    PRIVATE cgltf stb nlohmann_json::nlohmann_json Jolt meshoptimizer bc7enc miniaudio
            $<$<PLATFORM_ID:Windows>:shell32>)

# Project templates (installed next to the executables, see the root CMakeLists).
target_compile_definitions(Engine PRIVATE ENGINE_TEMPLATE_DIR="${CMAKE_SOURCE_DIR}/templates")

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

# Shader hot reload (development): recompile with the same compiler and flags as the build.
target_compile_definitions(Engine PRIVATE
    ENGINE_GLSLC="${GLSLC_EXECUTABLE}"
    ENGINE_SHADER_INCLUDE_DIR="${ENGINE_SHADER_INCLUDE_DIR}"
    ENGINE_SHADER_OPTIMIZE=$<IF:$<CONFIG:Debug>,0,1>)

engine_set_warnings(Engine)
set_target_properties(Engine PROPERTIES FOLDER "Engine")

# Shared GLSL includes (bindless.glsl, ...) show up in the IDE
set(ENGINE_SHADER_INCLUDES
    shaders/include/bindless.glsl
    shaders/include/depth_normal.glsl
    shaders/include/exposure.glsl
    shaders/include/frame.glsl
    shaders/include/lights.glsl
    shaders/include/ibl_common.glsl
    shaders/include/mesh_common.glsl
    shaders/include/mesh_shading.glsl
    shaders/include/pbr.glsl
    shaders/include/scene_common.glsl
    shaders/include/shadow.glsl
    shaders/include/sky.glsl
    shaders/include/surface.glsl)
target_sources(Engine PRIVATE ${ENGINE_SHADER_INCLUDES})
set_source_files_properties(${ENGINE_SHADER_INCLUDES} PROPERTIES HEADER_FILE_ONLY TRUE)

engine_add_shaders(Engine SOURCES
    shaders/bloom_downsample.comp
    shaders/bloom_upsample.comp
    shaders/depth_normal.frag
    shaders/depth_normal_id.frag
    shaders/exposure_average.comp
    shaders/fullscreen.vert
    shaders/gtao.comp
    shaders/gpu_cull.comp
    shaders/gtao_denoise.comp
    shaders/hiz.comp
    shaders/ibl_brdf.comp
    shaders/ibl_irradiance.comp
    shaders/ibl_prefilter.comp
    shaders/ibl_sky.comp
    shaders/light_cull.comp
    shaders/luminance_histogram.comp
    shaders/mesh.frag
    shaders/mesh_blend.frag
    shaders/mesh.vert
    shaders/scatter.comp
    shaders/shadow.vert
    shaders/shadow_mask.frag
    shaders/sky.frag
    shaders/text.frag
    shaders/text.vert
    shaders/tonemap.frag)
