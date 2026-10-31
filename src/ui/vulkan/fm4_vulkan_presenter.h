/**
 * @file        ui/vulkan/fm4_vulkan_presenter.h
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

// FM4's additions to VulkanPresenter: vulkan_present_mode and DXGI
// presentation (vulkan_present_via_dxgi), kept out of vulkan_presenter.cpp so
// that upstream's changes there merge cleanly. Private to the UI library.

#include <string>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/presenter.h>

#if REX_PLATFORM_WIN32
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>
#endif

REXCVAR_DECLARE(std::string, vulkan_present_mode);
#if REX_PLATFORM_WIN32
REXCVAR_DECLARE(bool, vulkan_allow_present_mode_immediate);
REXCVAR_DECLARE(bool, vulkan_allow_present_mode_mailbox);
#endif

namespace rex::ui::vulkan {

// vulkan_present_mode, VK_PRESENT_MODE_MAX_ENUM_KHR for auto.
VkPresentModeKHR RequestedPresentMode(const std::string& mode);

#if REX_PLATFORM_WIN32
// Vulkan draws each frame into one of a ring of textures shared with a
// Direct3D 11 device on the same adapter, and Direct3D 11 copies it to the
// back buffer of a DXGI flip-model swapchain and presents. Two shared fences
// order the APIs: `rendered` (Vulkan signals frame N drawn, Direct3D waits for
// it) and `copied` (Direct3D signals frame N copied, Vulkan waits for it before
// drawing into the same texture again, kImageCount frames later).
class VulkanPresenter::PaintContext::DxgiSwapchain {
 public:
  static constexpr uint32_t kImageCount = kSubmissionCount;
  static constexpr VkFormat kFormat = VK_FORMAT_B8G8R8A8_UNORM;

  static std::unique_ptr<DxgiSwapchain, DxgiSwapchainDeleter> Create(
      const VulkanDevice* vulkan_device, HWND hwnd, uint32_t width, uint32_t height) {
    const VulkanDevice::Extensions& extensions = vulkan_device->extensions();
    const VulkanDevice::Properties& properties = vulkan_device->properties();
    if (!extensions.ext_KHR_external_memory_win32 ||
        !extensions.ext_KHR_external_semaphore_win32 || !properties.timelineSemaphore ||
        !properties.deviceLUIDValid) {
      REXLOG_WARN(
          "VulkanPresenter: The Vulkan device doesn't support Win32 external memory, Win32 "
          "external semaphores, timeline semaphores or the device LUID, needed for DXGI "
          "presentation");
      return nullptr;
    }
    std::unique_ptr<DxgiSwapchain, DxgiSwapchainDeleter> dxgi_swapchain(
        new DxgiSwapchain(vulkan_device, hwnd));
    if (!dxgi_swapchain->Initialize(width, height)) {
      return nullptr;
    }
    return dxgi_swapchain;
  }

  DxgiSwapchain(const DxgiSwapchain&) = delete;
  DxgiSwapchain& operator=(const DxgiSwapchain&) = delete;

  ~DxgiSwapchain() {
    // The Vulkan side must have been awaited by the owner.
    AwaitDirect3DCompletion();
    DestroyImages();
    const VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    util::DestroyAndNullHandle(dfn.vkDestroySemaphore, device, rendered_semaphore_);
    util::DestroyAndNullHandle(dfn.vkDestroySemaphore, device, copied_semaphore_);
    swapchain_.Reset();
    copied_fence_.Reset();
    rendered_fence_.Reset();
    context_.Reset();
    device_.Reset();
    if (fence_event_) {
      CloseHandle(fence_event_);
    }
    if (d3d11_module_) {
      FreeLibrary(d3d11_module_);
    }
  }

  HWND hwnd() const { return hwnd_; }
  const VkExtent2D& extent() const { return extent_; }
  bool is_vsync() const { return sync_interval_ != 0; }
  const std::array<VkImage, kImageCount>& images() const { return images_; }

  // For the Vulkan submission drawing the next frame: the image to draw into,
  // the `copied` value to wait for before drawing into it, and the `rendered`
  // value to signal.
  uint64_t next_frame() const { return frame_ + 1; }
  uint32_t next_image_index() const { return uint32_t(next_frame() % kImageCount); }
  uint64_t next_image_copied_frame() const {
    return next_frame() > kImageCount ? next_frame() - kImageCount : 0;
  }

  // Neither API may be using the images (the owner awaits Vulkan).
  bool Resize(uint32_t width, uint32_t height) {
    AwaitDirect3DCompletion();
    DestroyImages();
    HRESULT result =
        swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapchain_flags_);
    if (FAILED(result)) {
      REXLOG_ERROR("VulkanPresenter: Failed to resize the DXGI swapchain buffers (0x{:08X})",
                   uint32_t(result));
      return false;
    }
    return CreateImages(width, height);
  }

  // To be called after the Vulkan submission that signals `rendered` for
  // next_frame() has succeeded.
  bool Present() {
    ++frame_;
    context_->Wait(rendered_fence_.Get(), frame_);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT result = swapchain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (SUCCEEDED(result)) {
      context_->CopyResource(back_buffer.Get(), textures_[frame_ % kImageCount].Get());
    }
    // Signaled even without the copy, as Vulkan will wait for this value.
    context_->Signal(copied_fence_.Get(), frame_);
    if (FAILED(result)) {
      context_->Flush();
      REXLOG_ERROR("VulkanPresenter: Failed to get the DXGI swapchain back buffer (0x{:08X})",
                   uint32_t(result));
      return false;
    }
    result = swapchain_->Present(sync_interval_, present_flags_);
    if (FAILED(result)) {
      REXLOG_ERROR("VulkanPresenter: Failed to present the DXGI swapchain (0x{:08X})",
                   uint32_t(result));
      return false;
    }
    return true;
  }

  // Releases the drawn image to Direct3D, which copies it to the back buffer.
  // No acquire is needed when drawing into it again, as its contents are
  // discarded then (the render pass starts from UNDEFINED).
  void RecordRelease(VkCommandBuffer command_buffer, uint32_t image_index) const {
    VkImageMemoryBarrier release_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    release_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    release_barrier.dstAccessMask = 0;
    release_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    release_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    release_barrier.srcQueueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
    release_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    release_barrier.image = images_[image_index];
    release_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    release_barrier.subresourceRange.levelCount = 1;
    release_barrier.subresourceRange.layerCount = 1;
    vulkan_device_->functions().vkCmdPipelineBarrier(
        command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &release_barrier);
  }

  // Makes the submission drawing the next frame wait for its image to be
  // copied (in place of the acquire semaphore) and signal it drawn (in place
  // of the present semaphore). Valid until the next call.
  void ChainSubmission(VkSubmitInfo& submit_info) {
    submit_copied_frame_ = next_image_copied_frame();
    submit_rendered_frame_ = next_frame();
    submit_timeline_info_ = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    submit_timeline_info_.waitSemaphoreValueCount = 1;
    submit_timeline_info_.pWaitSemaphoreValues = &submit_copied_frame_;
    submit_timeline_info_.signalSemaphoreValueCount = 1;
    submit_timeline_info_.pSignalSemaphoreValues = &submit_rendered_frame_;
    submit_info.pNext = &submit_timeline_info_;
    submit_info.pWaitSemaphores = &copied_semaphore_;
    submit_info.pSignalSemaphores = &rendered_semaphore_;
  }

 private:
  DxgiSwapchain(const VulkanDevice* vulkan_device, HWND hwnd)
      : vulkan_device_(vulkan_device), hwnd_(hwnd) {}

  bool Initialize(uint32_t width, uint32_t height) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
      REXLOG_ERROR("VulkanPresenter: Failed to create a DXGI factory (0x{:08X})",
                   uint32_t(result));
      return false;
    }
    LUID luid;
    static_assert(sizeof(luid) == VK_LUID_SIZE);
    std::memcpy(&luid, vulkan_device_->properties().deviceLUID, sizeof(luid));
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    result = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (FAILED(result)) {
      REXLOG_ERROR("VulkanPresenter: No DXGI adapter matches the Vulkan device (0x{:08X})",
                   uint32_t(result));
      return false;
    }

    // Loaded at runtime to keep Direct3D 11 out of the link.
    d3d11_module_ = LoadLibraryW(L"d3d11.dll");
    if (!d3d11_module_) {
      REXLOG_ERROR("VulkanPresenter: Failed to load d3d11.dll");
      return false;
    }
    auto d3d11_create_device = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
        reinterpret_cast<void*>(GetProcAddress(d3d11_module_, "D3D11CreateDevice")));
    if (!d3d11_create_device) {
      REXLOG_ERROR("VulkanPresenter: d3d11.dll has no D3D11CreateDevice");
      return false;
    }
    static const D3D_FEATURE_LEVEL kFeatureLevels[] = {D3D_FEATURE_LEVEL_11_1,
                                                       D3D_FEATURE_LEVEL_11_0};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    result = d3d11_create_device(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                 kFeatureLevels, UINT(rex::countof(kFeatureLevels)),
                                 D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(result)) {
      REXLOG_ERROR("VulkanPresenter: Failed to create a Direct3D 11 device (0x{:08X})",
                   uint32_t(result));
      return false;
    }
    if (FAILED(device.As(&device_)) || FAILED(context.As(&context_))) {
      REXLOG_ERROR(
          "VulkanPresenter: Direct3D 11.4 shared fences are unavailable (Windows 10 1703 or "
          "newer is needed)");
      return false;
    }

    BOOL tearing_supported = FALSE;
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory_5;
    if (SUCCEEDED(factory.As(&factory_5)) &&
        FAILED(factory_5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                              &tearing_supported, sizeof(tearing_supported)))) {
      tearing_supported = FALSE;
    }
    if (tearing_supported) {
      swapchain_flags_ |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    const VkPresentModeKHR requested_mode = RequestedPresentMode(REXCVAR_GET(vulkan_present_mode));
    if (requested_mode == VK_PRESENT_MODE_MAX_ENUM_KHR) {
      if (REXCVAR_GET(vulkan_allow_present_mode_immediate) && tearing_supported) {
        present_flags_ = DXGI_PRESENT_ALLOW_TEARING;
      } else if (!REXCVAR_GET(vulkan_allow_present_mode_immediate) &&
                 !REXCVAR_GET(vulkan_allow_present_mode_mailbox)) {
        sync_interval_ = 1;
      }
    } else if (requested_mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
      if (tearing_supported) {
        present_flags_ = DXGI_PRESENT_ALLOW_TEARING;
      }
    } else if (requested_mode == VK_PRESENT_MODE_FIFO_KHR) {
      sync_interval_ = 1;
    }

    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
    swapchain_desc.Width = width;
    swapchain_desc.Height = height;
    swapchain_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapchain_desc.SampleDesc.Count = 1;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = kImageCount;
    swapchain_desc.Scaling = DXGI_SCALING_STRETCH;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapchain_desc.Flags = swapchain_flags_;
    result = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &swapchain_desc, nullptr,
                                             nullptr, &swapchain_);
    if (FAILED(result)) {
      REXLOG_ERROR("VulkanPresenter: Failed to create a DXGI swapchain (0x{:08X})",
                   uint32_t(result));
      return false;
    }
    // Fullscreen is the window's business (borderless), not DXGI's.
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
      REXLOG_ERROR("VulkanPresenter: Failed to create the DXGI presentation fence event");
      return false;
    }
    if (!CreateSharedFence(rendered_fence_, rendered_semaphore_) ||
        !CreateSharedFence(copied_fence_, copied_semaphore_)) {
      return false;
    }

    if (!CreateImages(width, height)) {
      return false;
    }
    REXLOG_INFO("VulkanPresenter: Presenting through a {}x{} DXGI swapchain ({})", width, height,
                present_flags_ ? "tearing" : (sync_interval_ ? "vsync" : "no vsync"));
    return true;
  }

  bool CreateSharedFence(Microsoft::WRL::ComPtr<ID3D11Fence>& fence_out,
                         VkSemaphore& semaphore_out) {
    HRESULT result = device_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_out));
    if (FAILED(result)) {
      REXLOG_ERROR("VulkanPresenter: Failed to create a shared Direct3D 11 fence (0x{:08X})",
                   uint32_t(result));
      return false;
    }
    HANDLE handle;
    result = fence_out->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(result)) {
      REXLOG_ERROR("VulkanPresenter: Failed to share a Direct3D 11 fence (0x{:08X})",
                   uint32_t(result));
      return false;
    }
    const VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    VkSemaphoreTypeCreateInfo semaphore_type_create_info = {
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    semaphore_type_create_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore_create_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore_create_info.pNext = &semaphore_type_create_info;
    bool imported = false;
    if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr, &semaphore_out) ==
        VK_SUCCESS) {
      VkImportSemaphoreWin32HandleInfoKHR import_info = {
          VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
      import_info.semaphore = semaphore_out;
      import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
      import_info.handle = handle;
      imported = dfn.vkImportSemaphoreWin32HandleKHR(device, &import_info) == VK_SUCCESS;
    }
    // Importing doesn't take the ownership of an NT handle.
    CloseHandle(handle);
    if (!imported) {
      REXLOG_ERROR("VulkanPresenter: Failed to import a Direct3D 11 fence into Vulkan");
      return false;
    }
    return true;
  }

  bool CreateImages(uint32_t width, uint32_t height) {
    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texture_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    for (uint32_t i = 0; i < kImageCount; ++i) {
      HRESULT result = device_->CreateTexture2D(&texture_desc, nullptr, &textures_[i]);
      Microsoft::WRL::ComPtr<IDXGIResource1> resource;
      HANDLE handle = nullptr;
      if (SUCCEEDED(result)) {
        result = textures_[i].As(&resource);
      }
      if (SUCCEEDED(result)) {
        result = resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
      }
      if (FAILED(result)) {
        REXLOG_ERROR(
            "VulkanPresenter: Failed to create a shared Direct3D 11 texture (0x{:08X})",
            uint32_t(result));
        return false;
      }
      bool imported = ImportImage(handle, width, height, images_[i], memories_[i]);
      CloseHandle(handle);
      if (!imported) {
        return false;
      }
    }
    extent_.width = width;
    extent_.height = height;
    return true;
  }

  bool ImportImage(HANDLE handle, uint32_t width, uint32_t height, VkImage& image_out,
                   VkDeviceMemory& memory_out) {
    const VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    VkExternalMemoryImageCreateInfo external_create_info = {
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external_create_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo image_create_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_create_info.pNext = &external_create_info;
    image_create_info.imageType = VK_IMAGE_TYPE_2D;
    image_create_info.format = kFormat;
    image_create_info.extent.width = width;
    image_create_info.extent.height = height;
    image_create_info.extent.depth = 1;
    image_create_info.mipLevels = 1;
    image_create_info.arrayLayers = 1;
    image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_create_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (dfn.vkCreateImage(device, &image_create_info, nullptr, &image_out) != VK_SUCCESS) {
      REXLOG_ERROR("VulkanPresenter: Failed to create a Vulkan image for a shared texture");
      return false;
    }
    VkMemoryRequirements memory_requirements;
    dfn.vkGetImageMemoryRequirements(device, image_out, &memory_requirements);
    VkMemoryWin32HandlePropertiesKHR handle_properties = {
        VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    uint32_t memory_type;
    if (dfn.vkGetMemoryWin32HandlePropertiesKHR(device,
                                                VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
                                                handle, &handle_properties) != VK_SUCCESS ||
        !rex::bit_scan_forward(
            memory_requirements.memoryTypeBits & handle_properties.memoryTypeBits,
            &memory_type)) {
      REXLOG_ERROR("VulkanPresenter: No Vulkan memory type can import a shared texture");
      return false;
    }
    VkMemoryDedicatedAllocateInfo dedicated_allocate_info = {
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated_allocate_info.image = image_out;
    VkImportMemoryWin32HandleInfoKHR import_info = {
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    import_info.pNext = &dedicated_allocate_info;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    import_info.handle = handle;
    VkMemoryAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate_info.pNext = &import_info;
    allocate_info.allocationSize = memory_requirements.size;
    allocate_info.memoryTypeIndex = memory_type;
    if (dfn.vkAllocateMemory(device, &allocate_info, nullptr, &memory_out) != VK_SUCCESS ||
        dfn.vkBindImageMemory(device, image_out, memory_out, 0) != VK_SUCCESS) {
      REXLOG_ERROR("VulkanPresenter: Failed to import a shared texture into Vulkan");
      return false;
    }
    return true;
  }

  void DestroyImages() {
    const VulkanDevice::Functions& dfn = vulkan_device_->functions();
    const VkDevice device = vulkan_device_->device();
    for (uint32_t i = 0; i < kImageCount; ++i) {
      util::DestroyAndNullHandle(dfn.vkDestroyImage, device, images_[i]);
      util::DestroyAndNullHandle(dfn.vkFreeMemory, device, memories_[i]);
      textures_[i].Reset();
    }
    extent_ = {};
  }

  void AwaitDirect3DCompletion() {
    if (!frame_ || !copied_fence_ || copied_fence_->GetCompletedValue() >= frame_) {
      return;
    }
    context_->Flush();
    if (SUCCEEDED(copied_fence_->SetEventOnCompletion(frame_, fence_event_))) {
      WaitForSingleObject(fence_event_, INFINITE);
    }
  }

  const VulkanDevice* vulkan_device_;
  HWND hwnd_;

  HMODULE d3d11_module_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Device5> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
  UINT swapchain_flags_ = 0;
  UINT sync_interval_ = 0;
  UINT present_flags_ = 0;

  Microsoft::WRL::ComPtr<ID3D11Fence> rendered_fence_;
  Microsoft::WRL::ComPtr<ID3D11Fence> copied_fence_;
  VkSemaphore rendered_semaphore_ = VK_NULL_HANDLE;
  VkSemaphore copied_semaphore_ = VK_NULL_HANDLE;
  HANDLE fence_event_ = nullptr;
  // The last frame given to Direct3D, also the last value of both fences.
  uint64_t frame_ = 0;
  VkTimelineSemaphoreSubmitInfo submit_timeline_info_ = {};
  uint64_t submit_copied_frame_ = 0;
  uint64_t submit_rendered_frame_ = 0;

  VkExtent2D extent_ = {};
  std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kImageCount> textures_;
  std::array<VkImage, kImageCount> images_ = {};
  std::array<VkDeviceMemory, kImageCount> memories_ = {};
};
#endif  // REX_PLATFORM_WIN32

}  // namespace rex::ui::vulkan
