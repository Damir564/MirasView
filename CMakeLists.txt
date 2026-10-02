cmake_minimum_required (VERSION 3.21)

project ("MirasEngine")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
      "$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>"
      CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(
    $<$<CONFIG:Debug>:/Zi>
    $<$<CONFIG:RelWithDebInfo>:/Zi>
    $<$<CONFIG:Release>:/Zi>          # keep PDBs in Release too if you want to debug it
  )
  add_link_options(
    $<$<CONFIG:Debug>:/DEBUG>
    $<$<CONFIG:RelWithDebInfo>:/DEBUG>
  )
endif()

find_package(Vulkan REQUIRED)

# Ships a Vulkan loader, the Khronos emulation layers and a CPU driver in vulkan/ next to engine.exe, so the
# engine also runs on GPUs without Vulkan 1.3 / VK_EXT_shader_object and on PCs without a Vulkan driver.
# The x64-release preset turns it on.
option(MIRAS_BUNDLE_VULKAN_RUNTIME "Download and ship the fallback Vulkan runtime" OFF)

# Smaller release binaries: whole-program optimization (dependencies included) and per-variable
# COMDATs so the linker can drop unreferenced data as well as unreferenced functions.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
if (MSVC)
  add_compile_options("$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/Gw>")
endif()

add_subdirectory(external)

add_subdirectory(src)

file(COPY ${CMAKE_SOURCE_DIR}/src/shaders
     DESTINATION ${CMAKE_BINARY_DIR}/src)

# ==========================================
# Shader compilation CMake snippet
# ==========================================

# 1️⃣ Source and output directories
set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/src/shaders)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/src/shaders)

# Ensure output directory exists
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

# 2️⃣ Glob the shader stages; *.glsl files are shared includes and only act as dependencies.
file(GLOB SHADERS "${SHADER_SRC_DIR}/*.vert" "${SHADER_SRC_DIR}/*.frag" "${SHADER_SRC_DIR}/*.comp")
file(GLOB SHADER_INCLUDES "${SHADER_SRC_DIR}/*.glsl")

# List to hold SPIR-V output files
set(SPIRV_FILES "")

# 3️⃣ Loop through each shader and create a custom command
foreach(SHADER_FILE ${SHADERS})
    # Get filename without path
    get_filename_component(FILE_NAME_WE ${SHADER_FILE} NAME_WE)
    # Get the extension (vert, frag, etc.)
    get_filename_component(FILE_EXT ${SHADER_FILE} EXT)
    string(REPLACE "." "" FILE_EXT_CLEAN ${FILE_EXT}) # remove dot
    
    # Output file path
    set(OUTPUT_FILE ${SHADER_OUT_DIR}/${FILE_NAME_WE}.${FILE_EXT_CLEAN}.spv)
    
    # Add to SPIR-V list
    list(APPEND SPIRV_FILES ${OUTPUT_FILE})
    
    # Add custom command to compile shader
    add_custom_command(
        OUTPUT ${OUTPUT_FILE}
        COMMAND ${CMAKE_COMMAND} -E echo "Compiling ${SHADER_FILE} -> ${OUTPUT_FILE}"
        COMMAND glslangValidator -V ${SHADER_FILE} -I${SHADER_SRC_DIR} -o ${OUTPUT_FILE}
        DEPENDS ${SHADER_FILE} ${SHADER_INCLUDES}
        COMMENT "Compiling GLSL shader ${SHADER_FILE} to SPIR-V"
        VERBATIM
    )
endforeach()

# 4️⃣ Add a custom target to build all shaders
add_custom_target(Shaders ALL DEPENDS ${SPIRV_FILES})

# 5️⃣ Example: Link shaders to your executable target
add_dependencies(engine Shaders)

include(cmake/Packaging.cmake)


