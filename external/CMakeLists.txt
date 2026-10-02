project(external LANGUAGES CXX)

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(
  VulkanHpp
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Hpp.git
  GIT_TAG v1.4.336
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG 1.4.304
  GIT_SHALLOW TRUE
)

set(VOLK_HEADERS_ONLY OFF CACHE BOOL "" FORCE)
set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  VMA
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.3.0
)

set(VMA_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(VMA_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  vk_bootstrap
  GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
  GIT_TAG v1.4.336
  GIT_SHALLOW TRUE
)

set(VK_BOOTSTRAP_TEST OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.0
  GIT_SHALLOW TRUE
)

# Only windowing, events and Vulkan surfaces are used; drop the other subsystems and renderers.
foreach(SDL_UNUSED_OPTION
    SDL_AUDIO SDL_GPU SDL_RENDER SDL_CAMERA SDL_JOYSTICK SDL_HAPTIC SDL_HIDAPI SDL_POWER SDL_SENSOR
    SDL_DIALOG SDL_TRAY SDL_OPENGL SDL_OPENGLES SDL_TEST_LIBRARY)
  set(${SDL_UNUSED_OPTION} OFF CACHE BOOL "" FORCE)
endforeach()

FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.3
  GIT_SHALLOW TRUE
)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)


FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    fastgltf
    GIT_REPOSITORY https://github.com/spnda/fastgltf.git
    GIT_TAG v0.9.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.5-docking
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

# web-ifc (native IFC parsing + geometry). Its own CMakeLists always builds test executables and
# redeclares glm, so it and its header-only deps are only downloaded here ("SOURCE_SUBDIR none"
# skips add_subdirectory) and compiled as the web_ifc target below.
FetchContent_Declare(web_ifc
    GIT_REPOSITORY https://github.com/ThatOpen/engine_web-ifc.git
    GIT_TAG 0.78
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR none)
FetchContent_Declare(webifc_fastfloat GIT_REPOSITORY https://github.com/fastfloat/fast_float GIT_TAG b0ab987b3dfdde13fa1915f65ef2a5c068d9208c SOURCE_SUBDIR none)
FetchContent_Declare(webifc_tinynurbs GIT_REPOSITORY https://github.com/QuimMoya/tinynurbs GIT_TAG 47115cd9b6e922b27bbc4ab01fdeac2e9ea597a4 SOURCE_SUBDIR none)
# Stable earcut release instead of the master commit web-ifc pins: that revision corrupts its node
# ring on some wall profiles (null deref in Earcut::isEar) under MSVC. The API web-ifc uses is unchanged.
FetchContent_Declare(webifc_earcut GIT_REPOSITORY https://github.com/mapbox/earcut.hpp GIT_TAG v2.2.4 SOURCE_SUBDIR none)
FetchContent_Declare(webifc_cdt GIT_REPOSITORY https://github.com/artem-ogre/CDT GIT_TAG 2068d015b9db3c92481e869b0c1f669b96a1d70a SOURCE_SUBDIR none)
FetchContent_Declare(webifc_spdlog GIT_REPOSITORY https://github.com/gabime/spdlog GIT_TAG 79524ddd08a4ec981b7fea76afd08ee05f83755d SOURCE_SUBDIR none)
FetchContent_Declare(webifc_stduuid GIT_REPOSITORY https://github.com/mariusbancila/stduuid GIT_TAG 3afe7193facd5d674de709fccc44d5055e144d7a SOURCE_SUBDIR none)
FetchContent_Declare(webifc_unordered_dense GIT_REPOSITORY https://github.com/martinus/unordered_dense GIT_TAG 1c8636e810d7e4a485d17bb9cbd6dccb5fe4e284 SOURCE_SUBDIR none)

# Prebuilt Vulkan runtime shipped next to engine.exe (see cmake/Packaging.cmake): the loader, for PCs
# without a Vulkan driver, and Mesa's lavapipe, a Vulkan 1.4 driver that renders on the CPU. Only
# downloaded and extracted; SOURCE_SUBDIR none keeps FetchContent from looking for a CMake project.
if (MIRAS_BUNDLE_VULKAN_RUNTIME)
    FetchContent_Declare(vulkan_runtime
        URL https://sdk.lunarg.com/sdk/download/1.4.335.0/windows/VulkanRT-X64-1.4.335.0-Components.zip
        URL_HASH SHA256=4f856da356ffb1c16fbfc792cae70f6e6873680e6b62bc132b9d7131cbc97fe2
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR none)
    # Not 26.2.x: its lavapipe corrupts the heap in vkDestroySwapchainKHR on Windows (even vkcube crashes).
    FetchContent_Declare(mesa_dist
        URL https://github.com/pal1000/mesa-dist-win/releases/download/26.1.8/mesa3d-26.1.8-release-msvc.7z
        URL_HASH SHA256=4c6d32e653e0ff9ad07796e40c0bcfabf2764d849e3ce4f3b1590112c87e42f9
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR none)
    # The binary release has no license file; this is the MIT license that covers lavapipe.
    FetchContent_Declare(mesa_license
        URL https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-26.1.8/licenses/MIT
        URL_HASH SHA256=323c587d0ccf10e376f8bf9a7f31fb4ca6078105194b42e0b1e0ee2bc9bde71f
        DOWNLOAD_NO_EXTRACT TRUE
        SOURCE_SUBDIR none)
    FetchContent_MakeAvailable(vulkan_runtime mesa_dist mesa_license)
endif()

set(FASTGLTF_COMPILE_AS_CPP20 ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_compile_definitions(IMGUI_IMPL_VULKAN_NO_PROTOTYPES)


FetchContent_MakeAvailable(
  VulkanHpp
  volk
  VMA
  vk_bootstrap
  SDL3
  glm
  stb
  fastgltf
  imgui
  nlohmann_json
  web_ifc
  webifc_fastfloat
  webifc_tinynurbs
  webifc_earcut
  webifc_cdt
  webifc_spdlog
  webifc_stduuid
  webifc_unordered_dense
)

# SDL forces /GL- on SDL_mslibc.c, which conflicts with LTCG (D9025/C4652), so SDL is left out of it.
set_target_properties(SDL3-static PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

file(GLOB_RECURSE WEB_IFC_SOURCES CONFIGURE_DEPENDS ${web_ifc_SOURCE_DIR}/src/cpp/web-ifc/*.cpp)
add_library(web_ifc STATIC ${WEB_IFC_SOURCES})
set_target_properties(web_ifc PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(web_ifc
    # Public: web-ifc's own headers pull these in transitively.
    PUBLIC
        ${web_ifc_SOURCE_DIR}/src/cpp
        ${webifc_unordered_dense_SOURCE_DIR}/include
        ${webifc_fastfloat_SOURCE_DIR}/include
        ${webifc_tinynurbs_SOURCE_DIR}/include
        ${webifc_earcut_SOURCE_DIR}/include
        ${webifc_cdt_SOURCE_DIR}/CDT/include
        ${webifc_spdlog_SOURCE_DIR}/include
        ${webifc_stduuid_SOURCE_DIR}/include
)
target_link_libraries(web_ifc PUBLIC glm::glm-header-only)
if (MSVC)
    # Third-party code: silence its warnings; the schema tables need /bigobj.
    target_compile_options(web_ifc PRIVATE /utf-8 /bigobj /W0)
    target_compile_definitions(web_ifc PRIVATE _USE_MATH_DEFINES NOMINMAX)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} 
    ${imgui_SOURCE_DIR}/backends
)

add_library(stb_image STATIC
    stb_image.cpp
)

target_include_directories(stb_image
    PUBLIC ${stb_SOURCE_DIR}
)

target_link_libraries(imgui PUBLIC
    SDL3::SDL3
    Vulkan::Headers 
)
