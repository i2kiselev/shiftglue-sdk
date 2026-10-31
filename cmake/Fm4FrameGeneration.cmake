# Source-matched FSR Vulkan frame generation for the Windows Vulkan backend.
# The dependency retains its MIT / AMD notices in thirdparty/fm4-fsr-vulkan.
option(REXGLUE_ENABLE_FSR_FRAMEGENERATION
    "Build FSR 3.1.6 frame generation with the Windows Vulkan backend" ON)

set(REXGLUE_HAS_FSR_FRAMEGENERATION OFF CACHE INTERNAL "" FORCE)
if(NOT REXSDK_DIR)
    set(REXSDK_DIR "${PROJECT_SOURCE_DIR}")
endif()
if(NOT REXGLUE_ENABLE_FSR_FRAMEGENERATION OR NOT WIN32 OR NOT REXGLUE_USE_VULKAN)
    return()
endif()

# The renderer dynamically loads Vulkan. The standalone FSR backend needs the
# standard loader import library. Generate it from the pinned Khronos export
# definition so a separately installed Vulkan SDK is not a build prerequisite.
set(Vulkan_INCLUDE_DIR "${REXSDK_DIR}/thirdparty/vulkan-headers/include" CACHE PATH "" FORCE)
get_filename_component(_fm4_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(_fm4_vulkan_import "${CMAKE_CURRENT_BINARY_DIR}/fm4-vulkan-loader.lib")
if(NOT EXISTS "${_fm4_vulkan_import}")
    execute_process(
        COMMAND "${_fm4_llvm_bin}/llvm-dlltool.exe" -m i386:x86-64
            -d "${REXSDK_DIR}/thirdparty/vulkan-loader/loader/vulkan-1.def"
            -l "${_fm4_vulkan_import}"
        COMMAND_ERROR_IS_FATAL ANY)
endif()
set(Vulkan_LIBRARY "${_fm4_vulkan_import}" CACHE FILEPATH "" FORCE)
set(REXGLUE_FSR_VULKAN_IMPORT_LIBRARY "${_fm4_vulkan_import}" CACHE INTERNAL "" FORCE)

set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_FSR3_HOST OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_FSR3_VK_BACKEND OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_FSR3_3_1_5_SCAFFOLD ON CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_FSR4_V07_VULKAN OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${REXSDK_DIR}/thirdparty/fm4-fsr-vulkan" fm4-fsr-vulkan EXCLUDE_FROM_ALL)
set(REXGLUE_HAS_FSR_FRAMEGENERATION ON CACHE INTERNAL "" FORCE)

function(fm4_stage_rendering_notices target_name)
    set(_notice_root "$<TARGET_FILE_DIR:${target_name}>/rendering-notices")
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_notice_root}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${REXSDK_DIR}/thirdparty/fm4-fsr-vulkan/LICENSE.txt" "${_notice_root}/FSR-Vulkan-LICENSE.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${REXSDK_DIR}/thirdparty/fm4-fsr-vulkan/NOTICE.md" "${_notice_root}/FSR-Vulkan-NOTICE.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${REXSDK_DIR}/thirdparty/fm4-fsr-vulkan/UPSTREAM.md" "${_notice_root}/FSR-Vulkan-UPSTREAM.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${REXSDK_DIR}/thirdparty/fm4-fsr-vulkan/upstream/ffx-1.1.4/sdk/LICENSE.txt"
            "${_notice_root}/AMD-FidelityFX-1.1.4-LICENSE.txt"
        VERBATIM)
endfunction()
