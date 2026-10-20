#include "fm4_vulkan_presenter.h"

#if REX_PLATFORM_WIN32
#include <rex/ui/surface_win.h>
#endif

REXCVAR_DEFINE_STRING(vulkan_present_mode, "auto", "UI/Vulkan",
                      "Present mode: auto (by vsync and the vulkan_allow_present_mode_* "
                      "switches), immediate (tears; under variable refresh rate the smoothest), "
                      "fifo (vsync) or mailbox; one the driver lacks falls back to auto")
    .allowed({"auto", "immediate", "fifo", "mailbox"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#if REX_PLATFORM_WIN32
REXCVAR_DEFINE_BOOL(vulkan_present_via_dxgi, false, "UI/Vulkan",
                    "Present through a DXGI flip-model swapchain instead of the Vulkan one, so "
                    "that frame generation and capture tools (Lossless Scaling, NVIDIA Smooth "
                    "Motion, AMD Fluid Motion Frames, overlays) see the frames; falls back to the "
                    "Vulkan swapchain if unavailable. The allowed present modes map to DXGI: "
                    "immediate to tearing, mailbox to no vsync without tearing, else vsync")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DECLARE(bool, present_hdr);
#endif

namespace rex::ui::vulkan {

VkPresentModeKHR RequestedPresentMode(const std::string& mode) {
  if (mode == "immediate") {
    return VK_PRESENT_MODE_IMMEDIATE_KHR;
  }
  if (mode == "fifo") {
    return VK_PRESENT_MODE_FIFO_KHR;
  }
  if (mode == "mailbox") {
    return VK_PRESENT_MODE_MAILBOX_KHR;
  }
  return VK_PRESENT_MODE_MAX_ENUM_KHR;
}

#if REX_PLATFORM_WIN32
void VulkanPresenter::PaintContext::DxgiSwapchainDeleter::operator()(
    DxgiSwapchain* dxgi_swapchain) const {
  delete dxgi_swapchain;
}

// After an alt-tab drops a tearing window from independent flip back to
// composition, Nvidia can block every present for a second with nothing
// reaching the screen, until the desktop recomposes the window (a focus change
// or a resize). A stalled present shrinks the window by a pixel and the next one
// restores it: both in one go are merged before the desktop sees them.
void VulkanPresenter::PaintContext::RecomposeWindowIfPresentStalled(
    std::chrono::steady_clock::time_point present_start) {
  const auto present_time = std::chrono::steady_clock::now() - present_start;
  const HWND window = static_cast<HWND>(hwnd);
  // Asynchronous, as the window's thread may be waiting to paint.
  constexpr UINT kFlags =
      SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOOWNERZORDER | SWP_NOZORDER;
  if (stall_restore_width) {
    SetWindowPos(window, nullptr, 0, 0, stall_restore_width, stall_restore_height, kFlags);
    stall_restore_width = 0;
    return;
  }
  RECT rect;
  if (!window || present_time < std::chrono::milliseconds(250) || !GetWindowRect(window, &rect)) {
    return;
  }
  REXLOG_WARN("VulkanPresenter: A present took {} ms, resizing the window to get it recomposed",
              std::chrono::duration_cast<std::chrono::milliseconds>(present_time).count());
  stall_restore_width = rect.right - rect.left;
  stall_restore_height = rect.bottom - rect.top;
  SetWindowPos(window, nullptr, 0, 0, stall_restore_width, stall_restore_height - 1, kFlags);
}

bool VulkanPresenter::ConnectDxgiSwapchain(Surface& surface, uint32_t width, uint32_t height,
                                           bool present_mode_changed, VkFormat& format_out) {
  const bool win32_surface = surface.GetType() == Surface::kTypeIndex_Win32Hwnd;
  const HWND hwnd =
      win32_surface ? static_cast<const Win32HwndSurface&>(surface).hwnd() : nullptr;
  paint_context_.hwnd = hwnd;
  // HDR output needs the Vulkan swapchain's color spaces, and Resize keeps the
  // present mode the DXGI swapchain was made for.
  if (paint_context_.dxgi_swapchain && (REXCVAR_GET(present_hdr) || present_mode_changed)) {
    paint_context_.DestroySwapchainAndVulkanSurface();
  }
  if (!REXCVAR_GET(vulkan_present_via_dxgi) || REXCVAR_GET(present_hdr) ||
      paint_context_.dxgi_swapchain_failed || !win32_surface) {
    return false;
  }
  bool connected;
  if (paint_context_.dxgi_swapchain && paint_context_.dxgi_swapchain->hwnd() == hwnd) {
    // Awaits the Vulkan usage of the images and releases their framebuffers.
    paint_context_.PrepareForSwapchainRetirement();
    connected = paint_context_.dxgi_swapchain->Resize(width, height);
  } else {
    paint_context_.DestroySwapchainAndVulkanSurface();
    paint_context_.dxgi_swapchain =
        PaintContext::DxgiSwapchain::Create(vulkan_device_, hwnd, width, height);
    connected = paint_context_.dxgi_swapchain != nullptr;
  }
  if (!connected) {
    REXLOG_WARN(
        "VulkanPresenter: Presenting through DXGI failed, using the Vulkan swapchain for the "
        "rest of the session");
    paint_context_.DestroySwapchainAndVulkanSurface();
    paint_context_.dxgi_swapchain_failed = true;
    return false;
  }
  format_out = PaintContext::DxgiSwapchain::kFormat;
  paint_context_.swapchain_extent = paint_context_.dxgi_swapchain->extent();
  paint_context_.swapchain_is_fifo = paint_context_.dxgi_swapchain->is_vsync();
  return true;
}

Presenter::PaintResult VulkanPresenter::PresentDxgi() {
  const auto present_start = std::chrono::steady_clock::now();
  const bool presented = paint_context_.dxgi_swapchain->Present();
  paint_context_.RecomposeWindowIfPresentStalled(present_start);
  // A failure reconnects, and if the DXGI swapchain can't be rebuilt, the
  // Vulkan swapchain takes over.
  return presented ? PaintResult::kPresented : PaintResult::kNotPresentedConnectionOutdated;
}
#endif  // REX_PLATFORM_WIN32

}  // namespace rex::ui::vulkan
