# ReXGlue's runtime-loaded NVIDIA DLSS Neural Rendering bridge.
#
# The bridge links only NVIDIA's parameter allocator from the pinned DLSS SDK.
# NVIDIA's nvngx_dlssnr.dll model is user-provided and is never downloaded or
# installed by this project. Neural Rendering remains off unless the caller
# enables it and supplies that runtime.

option(REXGLUE_NR_BRIDGE
    "Build the optional runtime-loaded DLSS Neural Rendering bridge" ON)
set(REXGLUE_NR_RUNTIME_DLL "" CACHE FILEPATH
    "Optional user-installed nvngx_dlssnr.dll to stage with Neural Rendering")

set(REXGLUE_NR_BRIDGE_AVAILABLE OFF CACHE INTERNAL "" FORCE)

if(NOT REXGLUE_NR_BRIDGE)
    return()
endif()
if(NOT REXGLUE_USE_VULKAN OR NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(STATUS "Neural Rendering bridge: disabled (requires 64-bit Windows Vulkan)")
    return()
endif()
if(NOT REXGLUE_DLSS_INCLUDE_DIR OR NOT REXGLUE_DLSS_STATIC_LIBRARY
   OR NOT EXISTS "${REXGLUE_DLSS_STATIC_LIBRARY}")
    message(STATUS "Neural Rendering bridge: disabled (pinned DLSS SDK unavailable)")
    return()
endif()

set(_rexglue_nr_bridge_source
    "${REXGLUE_ROOT}/src/ui/vulkan/nr_bridge")
get_filename_component(_rexglue_nr_lib "${REXGLUE_DLSS_STATIC_LIBRARY}" NAME)
file(SHA256 "${REXGLUE_DLSS_STATIC_LIBRARY}" _rexglue_nr_lib_hash)
if(NOT _rexglue_nr_lib_hash STREQUAL
   "7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765")
    message(WARNING
        "Neural Rendering bridge: ${_rexglue_nr_lib} has an unexpected SHA-256; leaving bridge out")
    return()
endif()

add_library(bbport_nr SHARED
    "${_rexglue_nr_bridge_source}/bbport_nr_bridge.cpp"
    "${_rexglue_nr_bridge_source}/bbport_nr_bridge.h")
set_target_properties(bbport_nr PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    OUTPUT_NAME "bbport_nr_nvngx")
target_include_directories(bbport_nr PRIVATE
    "${REXGLUE_DLSS_INCLUDE_DIR}"
    "${_rexglue_nr_bridge_source}")
target_link_libraries(bbport_nr PRIVATE
    Vulkan::Headers
    "${REXGLUE_DLSS_STATIC_LIBRARY}")

set(REXGLUE_NR_BRIDGE_AVAILABLE ON CACHE INTERNAL "" FORCE)
set(REXGLUE_NR_BRIDGE_LICENSE
    "${_rexglue_nr_bridge_source}/LICENSE.txt"
    CACHE INTERNAL "")
set(REXGLUE_NR_NVIDIA_LICENSE "" CACHE INTERNAL "" FORCE)
if(REXGLUE_DLSS_RUNTIME_DIR AND
   EXISTS "${REXGLUE_DLSS_RUNTIME_DIR}/LICENSE.txt")
    set(_rexglue_nr_nvidia_license_dir "${CMAKE_BINARY_DIR}/licenses/bbport-nr")
    set(_rexglue_nr_nvidia_license
        "${_rexglue_nr_nvidia_license_dir}/NVIDIA-DLSS-LICENSE.txt")
    file(MAKE_DIRECTORY "${_rexglue_nr_nvidia_license_dir}")
    configure_file("${REXGLUE_DLSS_RUNTIME_DIR}/LICENSE.txt"
        "${_rexglue_nr_nvidia_license}" COPYONLY)
    set(REXGLUE_NR_NVIDIA_LICENSE
        "${_rexglue_nr_nvidia_license}"
        CACHE INTERNAL "" FORCE)
endif()
set(REXGLUE_NR_BRIDGE_SOURCE_DIR "${_rexglue_nr_bridge_source}"
    CACHE INTERNAL "")
message(STATUS "Neural Rendering bridge: enabled (runtime remains user-provided)")
