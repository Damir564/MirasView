add_executable (engine
  vma.cpp
  main.cpp
  vk_globals.cpp
  external/ImGuiFileDialog.h
  external/ImGuiFileDialogConfig.h
  external/ImGuiFileDialog.cpp
  engine/VulkanContext.h
  engine/VulkanContext.cpp
  engine/Swapchain.h
  engine/Swapchain.cpp
  engine/RenderTypes.h
  engine/GraphicsSettings.h
  engine/GraphicsSettings.cpp
  engine/ShaderUtils.h
  engine/ShaderUtils.cpp
  engine/Renderer.h
  engine/Renderer.cpp
  engine/RendererEffects.cpp
  engine/RenderUtils.h
  engine/Atmosphere.h
  engine/Atmosphere.cpp
  engine/Buffers.h
  engine/Log.h
  engine/Vertex.h
  engine/Camera.h
  engine/Shadow.h
  engine/Shadow.cpp
  engine/ModelTypes.h
  engine/ModelLoader.h
  engine/ModelLoader.cpp
  engine/ModelCache.h
  engine/ModelCache.cpp
  engine/ModelManager.h
  engine/ModelManager.cpp
  engine/Gizmo.h
  engine/SceneSerializer.h
  engine/SceneManager.h
  engine/SceneManager.cpp
  engine/CameraAnimation.h
  engine/CameraAnimation.cpp
  engine/IfcScene.h
  engine/IfcDirectLoader.h
  engine/IfcDirectLoader.cpp
  app/AppMode.h
  app/AppOptions.h
  app/AppOptions.cpp
  app/Application.h
  app/Application.cpp
  app/SettingsUi.h
  app/SettingsUi.cpp
  editor/Editor.h
  editor/Editor.cpp
  editor/EditorMenus.cpp
  editor/EditorHierarchy.cpp
  editor/EditorInspector.cpp
  editor/EditorAnnotations.cpp
  editor/EditorAnimation.cpp
  editor/EditorStatistics.cpp
  editor/EditorGizmo.cpp
  editor/EditorStyle.h
  editor/FileDialog.h
  game/Game.h
  game/Game.cpp
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET engine PROPERTY CXX_STANDARD 20)
endif()

target_sources(engine PRIVATE vma.cpp)

# Non-debug builds are GUI apps without a console window; keep main() as the entry point
# instead of WinMain. Their logging is compiled out as well (engine/Log.h).
set_property(TARGET engine PROPERTY WIN32_EXECUTABLE "$<NOT:$<CONFIG:Debug>>")
if (MSVC)
  target_link_options(engine PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>")
endif()

# Sources include each other as "engine/...", "app/...", "editor/...".
target_include_directories(engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Hot Reload (Edit and Continue) for our own code only.
if (POLICY CMP0141 AND MSVC)
  set_property(TARGET engine PROPERTY MSVC_DEBUG_INFORMATION_FORMAT
    "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

target_compile_definitions(engine PRIVATE
    VK_NO_PROTOTYPES    
    VULKAN_HPP_NO_EXCEPTIONS
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    USE_STD_FILESYSTEM
)

target_link_libraries(engine PRIVATE
    Vulkan::Headers
    volk
    VulkanMemoryAllocator
    vk-bootstrap::vk-bootstrap
    SDL3::SDL3
    glm::glm-header-only
    stb_image
    fastgltf::fastgltf
    imgui
    nlohmann_json::nlohmann_json
    web_ifc
)


