# Distributable build: `cmake --build --preset x64-release-package` writes MirasEngine-win64.zip into the build
# folder. The zip holds engine.exe with everything it loads from its folder:
#   shaders/    compiled SPIR-V
#   vulkan/     fallback Vulkan runtime (MIRAS_BUNDLE_VULKAN_RUNTIME), see VulkanContext
#   licenses/   third-party licenses
#   the MSVC runtime DLLs, and the scenes in MIRAS_PACKAGE_SCENES with the models they use.
# Everything is installed as the MirasEngine component so that the install rules of FetchContent
# dependencies (headers, static libraries) stay out of the package.

include(FetchContent)

set(MIRAS_EXE_DIR ${CMAKE_BINARY_DIR}/src)
set(MIRAS_COMPONENT MirasEngine)

# --- Fallback Vulkan runtime --------------------------------------------------------------------------
# Staged next to engine.exe in the build folder too, so builds run the same way the package does.
if (MIRAS_BUNDLE_VULKAN_RUNTIME)
    FetchContent_GetProperties(vulkan_runtime)
    FetchContent_GetProperties(mesa_dist)
    FetchContent_GetProperties(mesa_license)

    # The emulation layers come from the Vulkan SDK the engine is built with.
    set(MIRAS_SDK_BIN "$ENV{VULKAN_SDK}/Bin")
    if (Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
        get_filename_component(MIRAS_SDK_BIN "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" DIRECTORY)
    endif()
    set(MIRAS_RUNTIME_FILES
        ${vulkan_runtime_SOURCE_DIR}/x64/vulkan-1.dll
        ${mesa_dist_SOURCE_DIR}/x64/vulkan_lvp.dll
        ${MIRAS_SDK_BIN}/VkLayer_khronos_shader_object.dll
        ${MIRAS_SDK_BIN}/VkLayer_khronos_shader_object.json
        ${MIRAS_SDK_BIN}/VkLayer_khronos_synchronization2.dll
        ${MIRAS_SDK_BIN}/VkLayer_khronos_synchronization2.json)
    foreach(file IN LISTS MIRAS_RUNTIME_FILES)
        if (NOT EXISTS ${file})
            message(FATAL_ERROR "Vulkan runtime file not found: ${file}")
        endif()
    endforeach()
    file(COPY ${MIRAS_RUNTIME_FILES} DESTINATION ${MIRAS_EXE_DIR}/vulkan)

    install(DIRECTORY ${MIRAS_EXE_DIR}/vulkan DESTINATION . COMPONENT ${MIRAS_COMPONENT})
    install(FILES ${vulkan_runtime_SOURCE_DIR}/VulkanRT-License.txt
        DESTINATION licenses RENAME Vulkan-Loader.txt COMPONENT ${MIRAS_COMPONENT})
    install(FILES ${MIRAS_SDK_BIN}/../Licenses/LICENSE.txt
        DESTINATION licenses RENAME Vulkan-SDK-layers.txt COMPONENT ${MIRAS_COMPONENT} OPTIONAL)
    install(FILES ${mesa_license_SOURCE_DIR}/MIT
        DESTINATION licenses RENAME Mesa-lavapipe.txt COMPONENT ${MIRAS_COMPONENT})
endif()

# --- Engine -------------------------------------------------------------------------------------------
install(TARGETS engine RUNTIME DESTINATION . COMPONENT ${MIRAS_COMPONENT})
install(DIRECTORY ${MIRAS_EXE_DIR}/shaders/ DESTINATION shaders COMPONENT ${MIRAS_COMPONENT}
    FILES_MATCHING PATTERN "*.spv")

# msvcp140/vcruntime140 app-locally, so no VC++ redistributable install is needed. The SDK layers use them too.
# They must be at least as new as the compiler, so they come from the Visual Studio the compiler belongs to.
# Not InstallRequiredSystemLibraries: it doesn't know VS 2026's Microsoft.VC145.CRT and picks an older VS's.
string(REGEX REPLACE "/Tools/MSVC/.*$" "" MIRAS_VC_DIR "${CMAKE_CXX_COMPILER}")
file(GLOB MIRAS_CRT_DIRS LIST_DIRECTORIES true "${MIRAS_VC_DIR}/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
if (MIRAS_CRT_DIRS)
    list(SORT MIRAS_CRT_DIRS COMPARE NATURAL ORDER DESCENDING)
    list(GET MIRAS_CRT_DIRS 0 MIRAS_CRT_DIR)
    file(GLOB MIRAS_CRT_DLLS "${MIRAS_CRT_DIR}/msvcp140*.dll" "${MIRAS_CRT_DIR}/vcruntime140*.dll"
        "${MIRAS_CRT_DIR}/concrt140.dll")
    install(FILES ${MIRAS_CRT_DLLS} DESTINATION . COMPONENT ${MIRAS_COMPONENT})
elseif (CMAKE_BUILD_TYPE STREQUAL "Release")
    message(WARNING "No MSVC runtime found under ${MIRAS_VC_DIR}/Redist; the package will need the VC++ redistributable")
endif()

# --- Game content -------------------------------------------------------------------------------------
# Scenes next to engine.exe in the build folder (where the editor saves them), shipped with exactly the models
# they reference. Resolved when installing, so the package always matches the scenes as last saved.
set(MIRAS_PACKAGE_SCENES "level1.scn" CACHE STRING
    "Scenes, relative to engine.exe in the build folder, that the package ships along with the models they use")
option(MIRAS_PACKAGE_MODEL_CACHES
    "Also ship the models' .cache files: a few times bigger package, but no import on the first load" ON)
install(CODE "
    set(MIRAS_EXE_DIR [[${MIRAS_EXE_DIR}]])
    set(MIRAS_SCENES [[${MIRAS_PACKAGE_SCENES}]])
    set(MIRAS_MODEL_CACHES ${MIRAS_PACKAGE_MODEL_CACHES})"
    COMPONENT ${MIRAS_COMPONENT})
install(SCRIPT ${CMAKE_CURRENT_LIST_DIR}/PackageScenes.cmake COMPONENT ${MIRAS_COMPONENT})

# --- Third-party licenses -----------------------------------------------------------------------------
foreach(dep vulkanhpp volk vma vk_bootstrap sdl3 glm stb fastgltf imgui nlohmann_json web_ifc webifc_fastfloat
        webifc_tinynurbs webifc_earcut webifc_cdt webifc_spdlog webifc_stduuid webifc_unordered_dense)
    FetchContent_GetProperties(${dep})
    if (NOT ${dep}_SOURCE_DIR)
        continue()
    endif()
    file(GLOB licenses LIST_DIRECTORIES false
        ${${dep}_SOURCE_DIR}/LICENSE* ${${dep}_SOURCE_DIR}/LICENCE* ${${dep}_SOURCE_DIR}/COPYING*)
    if (licenses)
        install(FILES ${licenses} DESTINATION licenses/${dep} COMPONENT ${MIRAS_COMPONENT})
    endif()
endforeach()
# ImGuiFileDialog is vendored without a license file; its license is the header comment.
file(READ ${CMAKE_SOURCE_DIR}/src/external/ImGuiFileDialog.h header LIMIT 4096)
string(FIND "${header}" "MIT License" licenseBegin)
string(FIND "${header}" "*/" licenseEnd)
if (licenseBegin GREATER_EQUAL 0 AND licenseEnd GREATER licenseBegin)
    math(EXPR licenseLength "${licenseEnd} - ${licenseBegin}")
    string(SUBSTRING "${header}" ${licenseBegin} ${licenseLength} license)
    file(WRITE ${CMAKE_BINARY_DIR}/licenses/ImGuiFileDialog.txt "${license}")
    install(FILES ${CMAKE_BINARY_DIR}/licenses/ImGuiFileDialog.txt DESTINATION licenses COMPONENT ${MIRAS_COMPONENT})
endif()

# --- Zip ----------------------------------------------------------------------------------------------
# Not CPack: its ZIP writer stores names in CP437 and fails on the Cyrillic names of IFC models. Windows'
# own tar (bsdtar) writes them as UTF-8, which Explorer and Expand-Archive read.
find_program(MIRAS_TAR tar.exe PATHS "$ENV{SystemRoot}/System32" NO_DEFAULT_PATH)
set(MIRAS_PACKAGE_NAME MirasEngine-win64)
set(MIRAS_PACKAGE_STAGING ${CMAKE_BINARY_DIR}/package)
if (MIRAS_TAR)
    add_custom_target(dist
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${MIRAS_PACKAGE_STAGING} ${CMAKE_BINARY_DIR}/${MIRAS_PACKAGE_NAME}.zip
        COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --component ${MIRAS_COMPONENT}
            --prefix ${MIRAS_PACKAGE_STAGING}/${MIRAS_PACKAGE_NAME}
        COMMAND ${MIRAS_TAR} --options hdrcharset=UTF-8 -a -c -f ${CMAKE_BINARY_DIR}/${MIRAS_PACKAGE_NAME}.zip
            -C ${MIRAS_PACKAGE_STAGING} ${MIRAS_PACKAGE_NAME}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Packaging ${CMAKE_BINARY_DIR}/${MIRAS_PACKAGE_NAME}.zip"
        VERBATIM)
    add_dependencies(dist engine Shaders)
endif()
