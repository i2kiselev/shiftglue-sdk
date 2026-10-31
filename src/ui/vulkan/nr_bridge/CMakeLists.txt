# SPDX-FileCopyrightText: Copyright 2026 bbport contributors, bbhost contributors
# SPDX-License-Identifier: MIT
#
# bbport_nr_nvngx.dll links the NVIDIA DLSS SDK's parameter allocator (MSVC only) and is built
# separately from the port, exactly like bbport_dlss.dll:
#
#   cmake -S gpu/nr_bridge -B out/nr-bridge -G "Visual Studio 17 2022" -A x64 ^
#         -DDLSS_SDK_ROOT=<github.com/NVIDIA/DLSS checkout> ^
#         -DVULKAN_INCLUDE=<Vulkan-Headers include> ^
#         -DBBPORT_NR_BUILD_SMOKE=ON
#   cmake --build out/nr-bridge --config Release
#
# The smoke test is a standalone headless Vulkan program: it builds the bridge, runs the model over
# a synthetic frame and checks that the pixels actually changed. It is the only GPU test in this
# directory and it does not touch the port.

cmake_minimum_required(VERSION 3.20)
project(bbport_nr LANGUAGES CXX)

set(DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK root (contains include/ and lib/)")
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan-Headers include directory")
set(BBPORT_NR_SDK_LIBRARY
    "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
    CACHE FILEPATH "Static-CRT NVIDIA parameter allocator")
set(BBPORT_NR_SDK_LIB_SHA256
    "7B9A5239C1145AF5F645AB472AA066F9EA79DB36AC819562312AAE69D65F3765"
    CACHE STRING "Expected SHA-256 of BBPORT_NR_SDK_LIBRARY")
set(BBPORT_NR_STAGE_DIR "" CACHE PATH
    "Optional directory to receive bbport_nr_nvngx.dll after the build")
set(BBPORT_NR_RUNTIME_DLL "" CACHE FILEPATH
    "Optional nvngx_dlssnr.dll to copy to BBPORT_NR_STAGE_DIR")
option(BBPORT_NR_BUILD_SMOKE "Build the standalone Vulkan smoke test" OFF)

if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The Neural Rendering bridge is Windows x64 only")
endif()
if (NOT EXISTS "${DLSS_SDK_ROOT}/include/nvsdk_ngx_vk.h")
    message(FATAL_ERROR "Set DLSS_SDK_ROOT to the NVIDIA DLSS SDK (github.com/NVIDIA/DLSS)")
endif()
if (NOT EXISTS "${VULKAN_INCLUDE}/vulkan/vulkan.h")
    message(FATAL_ERROR "Set VULKAN_INCLUDE to a Vulkan-Headers include directory")
endif()
if (NOT EXISTS "${BBPORT_NR_SDK_LIBRARY}")
    message(FATAL_ERROR
        "Set BBPORT_NR_SDK_LIBRARY to the DLSS ${BBPORT_NR_SDK_LIB_SHA256} artifact")
endif()
file(SHA256 "${BBPORT_NR_SDK_LIBRARY}" _bbport_nr_sdk_lib_actual_sha256)
string(TOLOWER "${_bbport_nr_sdk_lib_actual_sha256}" _bbport_nr_sdk_lib_actual_sha256)
string(TOLOWER "${BBPORT_NR_SDK_LIB_SHA256}" _bbport_nr_sdk_lib_expected_sha256)
if (NOT _bbport_nr_sdk_lib_actual_sha256 STREQUAL _bbport_nr_sdk_lib_expected_sha256)
    message(FATAL_ERROR
        "Neural Rendering SDK library SHA-256 mismatch: expected "
        "${_bbport_nr_sdk_lib_expected_sha256}, got ${_bbport_nr_sdk_lib_actual_sha256}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# nvsdk_ngx_s.lib is the static-CRT (/MT) variant, and the bridge is loaded into a process that
# does not install a Visual C++ runtime for it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(bbport_nr SHARED bbport_nr_bridge.cpp bbport_nr_bridge.h)

# The file name is load bearing. The model resolves the module that owns its caller's return address
# and refuses any whose path does not contain "nvngx.dll", so the output has to be named for that
# whatever the target is called. Renaming it breaks Neural Rendering and nothing else.
set_target_properties(bbport_nr PROPERTIES OUTPUT_NAME "bbport_nr_nvngx")

target_include_directories(bbport_nr PRIVATE "${DLSS_SDK_ROOT}/include" "${VULKAN_INCLUDE}")
target_link_libraries(bbport_nr PRIVATE "${BBPORT_NR_SDK_LIBRARY}")

if (BBPORT_NR_STAGE_DIR)
    add_custom_command(TARGET bbport_nr POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${BBPORT_NR_STAGE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:bbport_nr>" "${BBPORT_NR_STAGE_DIR}")
    if (BBPORT_NR_RUNTIME_DLL)
        if (NOT EXISTS "${BBPORT_NR_RUNTIME_DLL}")
            message(FATAL_ERROR
                "BBPORT_NR_RUNTIME_DLL does not exist: ${BBPORT_NR_RUNTIME_DLL}")
        endif()
        add_custom_command(TARGET bbport_nr POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${BBPORT_NR_RUNTIME_DLL}" "${BBPORT_NR_STAGE_DIR}")
    endif()
endif()

if (BBPORT_NR_BUILD_SMOKE)
    # Vulkan is resolved through vulkan-1.dll at run time, so the smoke test needs no import library
    # and no Vulkan SDK install beyond the headers.
    add_executable(nr_smoke smoke/nr_smoke.cpp)
    target_include_directories(nr_smoke PRIVATE "${VULKAN_INCLUDE}" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(nr_smoke PROPERTIES OUTPUT_NAME "nr_smoke")
endif()
