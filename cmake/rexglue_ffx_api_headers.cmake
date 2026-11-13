# rexglue_ffx_api_headers.cmake — the FidelityFX API headers (MIT), without
# the SDK build.
#
# The FSR 2/3 provider (src/ui/vulkan/ffx_upscaler.cpp) calls the FidelityFX
# API through five entry points. Without REXGLUE_ENABLE_FIDELITYFX (the SDK
# built from source), it loads them at run time: on Windows from AMD's signed
# runtime (amd_fidelityfx_vk.dll, SDK 1.1.4), downloaded here from the same
# pinned fork commit, which carries it in PrebuiltSignedDLL/; elsewhere from
# the library thirdparty/fidelityfx-vk builds from source. The headers come
# from that commit too.
#
# Sets REXGLUE_FFX_API_INCLUDE_DIR when the headers are present, and on
# Windows REXGLUE_FFX_RUNTIME_DLL when the signed runtime is (the game
# stages it in upscalers/fidelityfx/1.1.4/, rexglue_helpers.cmake).
#
# On Windows it also fetches AMD's signed SDK 2.4.0 Vulkan runtime (the loader,
# the upscaler DLL: FSR 4.1.2, 3.1.6 and 2.3.4, and the frame generation DLL)
# from AMD's repository and sets REXGLUE_FFX_SDK2_RUNTIME_DLLS; the game stages them in
# upscalers/fidelityfx/2.4.0/, which the provider tries first. The 1.1.4
# runtime stays as the fallback. The headers stay at 1.1.4: the provider
# declares the one SDK 2.x descriptor it needs.

option(REXGLUE_FFX_API_LOADER
    "Build the FSR 2/3 provider that loads the FidelityFX runtime at run time" ON)

set(REXGLUE_FFX_API_INCLUDE_DIR "" CACHE INTERNAL "")
set(REXGLUE_FFX_RUNTIME_DLL "" CACHE INTERNAL "")
if(REXGLUE_FIDELITYFX_SOURCE_DIR)
    set(REXGLUE_FFX_API_INCLUDE_DIR "${REXGLUE_FIDELITYFX_SOURCE_DIR}/ffx-api/include"
        CACHE INTERNAL "")
    return()
endif()
if(NOT REXGLUE_FFX_API_LOADER OR NOT REXGLUE_USE_VULKAN)
    return()
endif()

set(_rexglue_ffx_api_commit eee08db1688ac3d1275a70b728f4a8ba22914213)
set(_rexglue_ffx_api_url
    "https://raw.githubusercontent.com/rexglue/FidelityFX-SDK/${_rexglue_ffx_api_commit}/ffx-api/include/ffx_api")
set(_rexglue_ffx_api_dir "${CMAKE_BINARY_DIR}/ffx-api-headers")
set(_rexglue_ffx_api_files
    "ffx_api.h|29061c00601017954d88074714ebc6917e3ee08a72ccee898b1d01450268da40"
    "ffx_api_types.h|dee2782813d22047c068e0fb27d1347324f8ce79d280a4672848b3b294cb12fe"
    "ffx_upscale.h|1155bcdcea9c10e6a95507bc9422ca062a853ac84250f0b09a637a66836b3ac1"
    "vk/ffx_api_vk.h|a50a8d5ff5689a10a6ce378f2df5569fd880c89ec46e8356aaf35eb40512edd9")
foreach(_entry IN LISTS _rexglue_ffx_api_files)
    string(REPLACE "|" ";" _entry "${_entry}")
    list(GET _entry 0 _file)
    list(GET _entry 1 _hash)
    set(_path "${_rexglue_ffx_api_dir}/ffx_api/${_file}")
    if(EXISTS "${_path}")
        file(SHA256 "${_path}" _actual)
        if(_actual STREQUAL _hash)
            continue()
        endif()
    endif()
    file(DOWNLOAD "${_rexglue_ffx_api_url}/${_file}" "${_path}"
        EXPECTED_HASH SHA256=${_hash} STATUS _status TLS_VERIFY ON)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        file(REMOVE "${_path}")
        message(WARNING
            "Could not download the FidelityFX API header ${_file} (${_status}); "
            "the FSR 2/3 provider is left out.")
        return()
    endif()
endforeach()
set(REXGLUE_FFX_API_INCLUDE_DIR "${_rexglue_ffx_api_dir}" CACHE INTERNAL "")

# AMD's signed runtime: the fork carries the SDK 1.1.4 release's DLL
# unchanged (MIT, redistributable with the notice).
if(WIN32 AND REXGLUE_FIDELITYFX_RUNTIME AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_rexglue_ffx_runtime "${_rexglue_ffx_api_dir}/runtime/amd_fidelityfx_vk.dll")
    set(_rexglue_ffx_runtime_hash
        a1624cc4238fef046f30c4d80ce3f47be63fc5f5373f49e3ee9edb9960f54c78)
    set(_rexglue_ffx_runtime_ok OFF)
    if(EXISTS "${_rexglue_ffx_runtime}")
        file(SHA256 "${_rexglue_ffx_runtime}" _actual)
        if(_actual STREQUAL _rexglue_ffx_runtime_hash)
            set(_rexglue_ffx_runtime_ok ON)
        endif()
    endif()
    if(NOT _rexglue_ffx_runtime_ok)
        file(DOWNLOAD
            "https://raw.githubusercontent.com/rexglue/FidelityFX-SDK/${_rexglue_ffx_api_commit}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll"
            "${_rexglue_ffx_runtime}" STATUS _status TLS_VERIFY ON)
        list(GET _status 0 _code)
        set(_actual "")
        if(_code EQUAL 0)
            file(SHA256 "${_rexglue_ffx_runtime}" _actual)
        endif()
        if(_actual STREQUAL _rexglue_ffx_runtime_hash)
            set(_rexglue_ffx_runtime_ok ON)
        else()
            file(REMOVE "${_rexglue_ffx_runtime}")
            message(WARNING "Could not download AMD's FidelityFX runtime (${_status}); "
                "FSR 2 and 3 show as unavailable.")
        endif()
    endif()
    if(_rexglue_ffx_runtime_ok)
        set(REXGLUE_FFX_RUNTIME_DLL "${_rexglue_ffx_runtime}" CACHE INTERNAL "")
    endif()
endif()

# AMD's signed SDK 2.4.0 Vulkan runtime (MIT, redistributable with the
# notice), from Kits/FidelityFX/signedbin of the v2.4.0 tag.
set(REXGLUE_FFX_SDK2_RUNTIME_DLLS "" CACHE INTERNAL "")
if(WIN32 AND REXGLUE_FIDELITYFX_RUNTIME AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_rexglue_ffx_sdk2_url
        "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin")
    set(_rexglue_ffx_sdk2_files
        "amd_fidelityfx_loader_vk.dll|38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9"
        "amd_fidelityfx_upscaler_vk.dll|4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7"
        "amd_fidelityfx_framegeneration_vk.dll|d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4")
    set(_rexglue_ffx_sdk2_paths "")
    foreach(_entry IN LISTS _rexglue_ffx_sdk2_files)
        string(REPLACE "|" ";" _entry "${_entry}")
        list(GET _entry 0 _file)
        list(GET _entry 1 _hash)
        set(_path "${_rexglue_ffx_api_dir}/runtime-2.4.0/${_file}")
        set(_actual "")
        if(EXISTS "${_path}")
            file(SHA256 "${_path}" _actual)
        endif()
        if(NOT _actual STREQUAL _hash)
            file(DOWNLOAD "${_rexglue_ffx_sdk2_url}/${_file}" "${_path}"
                EXPECTED_HASH SHA256=${_hash} STATUS _status TLS_VERIFY ON)
            list(GET _status 0 _code)
            if(NOT _code EQUAL 0)
                file(REMOVE "${_path}")
                message(WARNING "Could not download AMD's FidelityFX SDK 2.4.0 runtime "
                    "${_file} (${_status}); FSR 4 shows as unavailable.")
                set(_rexglue_ffx_sdk2_paths "")
                break()
            endif()
        endif()
        list(APPEND _rexglue_ffx_sdk2_paths "${_path}")
    endforeach()
    set(REXGLUE_FFX_SDK2_RUNTIME_DLLS "${_rexglue_ffx_sdk2_paths}" CACHE INTERNAL "")
endif()
