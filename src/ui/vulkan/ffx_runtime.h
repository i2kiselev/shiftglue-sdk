/**
 * The FidelityFX API entry points of the runtime vulkan_ffx_upscaler.cpp
 * loads, shared with frame generation (vulkan_ffx_frame_generation.cpp).
 * Internal to rexui: needs the FidelityFX API headers (REX_HAS_FFX_API).
 */

#pragma once

#if defined(REX_HAS_FFX_API) && REX_HAS_FFX_API

#include <ffx_api/ffx_api.h>

#include <rex/ui/vulkan/api.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;

// All null when the runtime is unavailable; configure may be null alone with
// a runtime that does not export it.
struct FfxApiEntryPoints {
  PfnFfxCreateContext create_context = nullptr;
  PfnFfxDestroyContext destroy_context = nullptr;
  PfnFfxConfigure configure = nullptr;
  PfnFfxQuery query = nullptr;
  PfnFfxDispatch dispatch = nullptr;
  bool loaded() const { return create_context && destroy_context && query && dispatch; }
};

FfxApiEntryPoints GetFfxApiEntryPoints();

// The vkGetDeviceProcAddr to give a FidelityFX Vulkan backend for this device
// (it also answers the KHR aliases the backend asks for).
PFN_vkGetDeviceProcAddr GetFfxDeviceProcAddr(const VulkanDevice* device);

}  // namespace vulkan
}  // namespace ui
}  // namespace rex

#endif  // defined(REX_HAS_FFX_API) && REX_HAS_FFX_API
