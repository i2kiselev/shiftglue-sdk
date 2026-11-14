/**
 * AMD FidelityFX frame generation through the FidelityFX API (SDK 2.4 Vulkan
 * runtime): the frame generation swapchain, which replaces the presenter's
 * VkSwapchainKHR and presents an interpolated frame before each rendered one,
 * and the frame generation context it dispatches.
 *
 * The SDK's headers stay at 1.1.4 (rexglue_ffx_api_headers.cmake), which carry
 * the frame generation swapchain's Vulkan structures but not the frame
 * generation ones (ffx_framegeneration.h), so those are declared here with
 * the layouts and type values of SDK 2.4.0's headers (MIT).
 */

#include <rex/ui/vulkan/frame_generation.h>

#include <atomic>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

#include "ffx_runtime.h"

#if defined(REX_HAS_FFX_API) && REX_HAS_FFX_API
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_types.h>
#include <ffx_api/vk/ffx_api_vk.h>
#endif

REXCVAR_DEFINE_BOOL(present_frame_generation_debug, false, "UI/Presenter",
                    "Frame generation: draw AMD's tear and reset indicators and pacing lines "
                    "into the presented frames");

namespace rex {
namespace ui {
namespace vulkan {

#if defined(REX_HAS_FFX_API) && REX_HAS_FFX_API
namespace {

// Effect IDs and versions of FidelityFX SDK 2.4.0.
constexpr uint64_t kTypeBackendVk = 0x01000003u;
constexpr uint64_t kTypeSwapchainCreate = 0x00040001u;
constexpr uint64_t kTypeSwapchainFunctions = 0x00040005u;
constexpr uint64_t kTypeSwapchainWaitForPresents = 0x00040007u;
constexpr uint64_t kTypeSwapchainVersion = 0x0004000au;
constexpr uint32_t kSwapchainVersion = (4u << 22) | (0u << 12) | 0u;
constexpr uint64_t kTypeFrameGenerationCreate = 0x00020001u;
constexpr uint64_t kTypeFrameGenerationConfigure = 0x00020002u;
constexpr uint64_t kTypeFrameGenerationPrepareV2 = 0x0002000cu;
constexpr uint64_t kTypeFrameGenerationVersion = 0x0002000eu;
constexpr uint32_t kFrameGenerationVersion = (4u << 22) | (0u << 12) | 2u;
constexpr uint32_t kFlagDrawDebugTearLines = 1u << 0;
constexpr uint32_t kFlagDrawDebugResetIndicators = 1u << 1;
constexpr uint32_t kFlagDrawDebugPacingLines = 1u << 4;

struct DeviceContext {
  VkDevice device;
  VkPhysicalDevice physical_device;
  PFN_vkGetDeviceProcAddr get_device_proc_addr;
};
struct BackendDesc {
  ffxCreateContextDescHeader header;
  DeviceContext device_context;
};
struct SwapchainCreateDesc {
  ffxCreateContextDescHeader header;
  VkPhysicalDevice physicalDevice;
  VkDevice device;
  VkSwapchainKHR* swapchain;
  VkAllocationCallbacks* allocator;
  VkSwapchainCreateInfoKHR createInfo;
  VkQueueInfoFFXAPI gameQueue;
  VkQueueInfoFFXAPI asyncComputeQueue;
  VkQueueInfoFFXAPI presentQueue;
  VkQueueInfoFFXAPI imageAcquireQueue;
};
struct PresentCallbackDesc {
  ffxDispatchDescHeader header;
  void* device;
  void* commandList;
  FfxApiResource currentBackBuffer;
  FfxApiResource currentUI;
  FfxApiResource outputSwapChainBuffer;
  bool isGeneratedFrame;
  uint64_t frameID;
};
using PfnPresentCallback = ffxReturnCode_t (*)(PresentCallbackDesc* params, void* user_context);
struct VersionDesc {
  ffxCreateContextDescHeader header;
  uint32_t version;
};
using PfnCreateSwapchainFfx = VkResult (*)(VkDevice, const VkSwapchainCreateInfoKHR*,
                                           const VkAllocationCallbacks*, VkSwapchainKHR*, void*);
using PfnDestroySwapchainFfx = void (*)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*,
                                        void*);
struct SwapchainFunctionsDesc {
  ffxQueryDescHeader header;
  PfnCreateSwapchainFfx create_swapchain;
  PfnDestroySwapchainFfx destroy_swapchain;
  PFN_vkGetSwapchainImagesKHR get_swapchain_images;
  PFN_vkAcquireNextImageKHR acquire_next_image;
  PFN_vkQueuePresentKHR queue_present;
  PFN_vkSetHdrMetadataEXT set_hdr_metadata;
  uint64_t (*get_last_present_count)(VkSwapchainKHR);
};
struct FrameGenerationCreateDesc {
  ffxCreateContextDescHeader header;
  uint32_t flags;
  FfxApiDimensions2D displaySize;
  FfxApiDimensions2D maxRenderSize;
  uint32_t backBufferFormat;
};
using PfnFrameGenerationDispatch = ffxReturnCode_t (*)(ffxDispatchDescHeader* params,
                                                       void* user_context);
struct FrameGenerationConfigureDesc {
  ffxConfigureDescHeader header;
  void* swapChain;
  PfnPresentCallback presentCallback;
  void* presentCallbackUserContext;
  PfnFrameGenerationDispatch frameGenerationCallback;
  void* frameGenerationCallbackUserContext;
  bool frameGenerationEnabled;
  bool allowAsyncWorkloads;
  FfxApiResource HUDLessColor;
  uint32_t flags;
  bool onlyPresentGenerated;
  FfxApiRect2D generationRect;
  uint64_t frameID;
};
struct FrameGenerationPrepareDesc {
  ffxDispatchDescHeader header;
  uint64_t frameID;
  uint32_t flags;
  void* commandList;
  FfxApiDimensions2D renderSize;
  FfxApiFloatCoords2D jitterOffset;
  FfxApiFloatCoords2D motionVectorScale;
  float frameTimeDelta;
  bool reset;
  float cameraNear;
  float cameraFar;
  float cameraFovAngleVertical;
  float viewSpaceToMetersFactor;
  FfxApiResource depth;
  FfxApiResource motionVectors;
  float cameraPosition[3];
  float cameraUp[3];
  float cameraRight[3];
  float cameraForward[3];
};

// The game queue is the presenter's, which the SDK locks around every use; the
// swapchain submits to it on present from the presenting thread.
std::atomic<const VulkanDevice*> g_game_device{nullptr};
std::atomic<VulkanDevice::Queue*> g_game_queue{nullptr};

VkResult GameQueueSubmit(uint32_t submit_count, const VkSubmitInfo* submits, VkFence fence) {
  const VulkanDevice* device = g_game_device.load(std::memory_order_acquire);
  VulkanDevice::Queue* queue = g_game_queue.load(std::memory_order_acquire);
  if (!device || !queue) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VulkanDevice::Queue::Acquisition acquisition = queue->Acquire();
  return device->functions().vkQueueSubmit(acquisition.queue(), submit_count, submits, fence);
}

ffxReturnCode_t DispatchFrameGeneration(ffxDispatchDescHeader* params, void* user_context) {
  const FfxApiEntryPoints entry_points = GetFfxApiEntryPoints();
  return entry_points.dispatch(static_cast<ffxContext*>(user_context), params);
}

FfxApiResource Resource(const VulkanUpscalerImage& image, uint32_t state) {
  VkImageCreateInfo create_info = {};
  create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  create_info.imageType = VK_IMAGE_TYPE_2D;
  create_info.format = image.format;
  create_info.extent = {image.width, image.height, 1};
  create_info.mipLevels = 1;
  create_info.arrayLayers = 1;
  create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  create_info.usage = image.usage;
  return ffxApiGetResourceVK(
      reinterpret_cast<void*>(image.image),
      ffxApiGetImageResourceDescriptionVK(image.image, create_info, FFX_API_RESOURCE_USAGE_READ_ONLY),
      state);
}

VkImageLayout LayoutOfState(uint32_t state) {
  if (state & FFX_API_RESOURCE_STATE_PRESENT) {
    return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  }
  if (state & FFX_API_RESOURCE_STATE_RENDER_TARGET) {
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  if (state & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) {
    return VK_IMAGE_LAYOUT_GENERAL;
  }
  if (state == FFX_API_RESOURCE_STATE_COPY_SRC) {
    return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  }
  if (state == FFX_API_RESOURCE_STATE_COPY_DEST) {
    return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  }
  if (state & (FFX_API_RESOURCE_STATE_PIXEL_READ | FFX_API_RESOURCE_STATE_COMPUTE_READ)) {
    return state & FFX_API_RESOURCE_STATE_COPY_SRC ? VK_IMAGE_LAYOUT_GENERAL
                                                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  return VK_IMAGE_LAYOUT_GENERAL;
}

VulkanFrameGeneration::PresentImage PresentImageOf(const FfxApiResource& resource) {
  VulkanFrameGeneration::PresentImage image;
  image.image = static_cast<VkImage>(resource.resource);
  image.width = resource.description.width;
  image.height = resource.description.height;
  image.layout = LayoutOfState(resource.state);
  return image;
}

VulkanDevice::Queue* QueueOf(const VulkanDevice* device, const VulkanDevice::QueueSlot& slot) {
  return device->queue_families()[slot.family].queues[slot.index].get();
}

class VulkanFfxFrameGeneration final : public VulkanFrameGeneration {
 public:
  VulkanFfxFrameGeneration(const VulkanDevice* device, FfxApiEntryPoints entry_points)
      : device_(device), ffx_(entry_points) {}
  ~VulkanFfxFrameGeneration() override {
    VkSwapchainKHR swapchain = swapchain_;
    ReleaseSwapchain(swapchain);
  }

  bool WrapSwapchain(VkSwapchainKHR& swapchain, const VkSwapchainCreateInfoKHR& create_info,
                     uint32_t game_queue_family) override {
    VkSwapchainKHR unwrapped = swapchain;
    ReleaseSwapchain(unwrapped);
    if (create_info.imageSharingMode != VK_SHARING_MODE_EXCLUSIVE) {
      REXLOG_WARN("Frame generation: the swapchain is shared between queue families; not used");
      return false;
    }
    const VulkanDevice::FrameGenerationQueues& queues = device_->frame_generation_queues();
    g_game_device.store(device_, std::memory_order_release);
    g_game_queue.store(device_->queue_families()[game_queue_family].queues[0].get(),
                       std::memory_order_release);

    auto queue_info = [&](const VulkanDevice::QueueSlot& slot) {
      VkQueueInfoFFXAPI info = {};
      info.queue = QueueOf(device_, slot)->queue;
      info.familyIndex = slot.family;
      info.submitFunc = nullptr;
      return info;
    };
    SwapchainCreateDesc create_desc = {};
    create_desc.header.type = kTypeSwapchainCreate;
    create_desc.physicalDevice = device_->physical_device();
    create_desc.device = device_->device();
    create_desc.swapchain = &swapchain;
    create_desc.allocator = nullptr;
    create_desc.createInfo = create_info;
    create_desc.createInfo.oldSwapchain = swapchain;
    create_desc.gameQueue.queue = g_game_queue.load()->queue;
    create_desc.gameQueue.familyIndex = game_queue_family;
    create_desc.gameQueue.submitFunc = GameQueueSubmit;
    create_desc.asyncComputeQueue = queue_info(queues.async_compute);
    create_desc.presentQueue = queue_info(queues.present);
    create_desc.imageAcquireQueue = queue_info(queues.image_acquire);
    VersionDesc version_desc = {};
    version_desc.header.type = kTypeSwapchainVersion;
    version_desc.version = kSwapchainVersion;
    create_desc.header.pNext = &version_desc.header;

    ffxContext swapchain_context = nullptr;
    const ffxReturnCode_t created =
        ffx_.create_context(&swapchain_context, &create_desc.header, nullptr);
    if (created != FFX_API_RETURN_OK) {
      REXLOG_WARN("Frame generation: could not create the frame generation swapchain: error {}",
                  created);
      // The runtime may have destroyed the swapchain it was to replace.
      swapchain = VK_NULL_HANDLE;
      return false;
    }
    swapchain_context_ = swapchain_context;
    swapchain_ = swapchain;

    SwapchainFunctionsDesc functions_desc = {};
    functions_desc.header.type = kTypeSwapchainFunctions;
    if (ffx_.query(&swapchain_context, &functions_desc.header) != FFX_API_RETURN_OK ||
        !functions_desc.get_swapchain_images || !functions_desc.acquire_next_image ||
        !functions_desc.queue_present || !functions_desc.destroy_swapchain) {
      REXLOG_WARN("Frame generation: the swapchain gave no replacement functions");
      destroy_swapchain_ = functions_desc.destroy_swapchain;
      ReleaseSwapchain(swapchain);
      return false;
    }
    destroy_swapchain_ = functions_desc.destroy_swapchain;
    functions_.get_swapchain_images = functions_desc.get_swapchain_images;
    functions_.acquire_next_image = functions_desc.acquire_next_image;
    functions_.queue_present = functions_desc.queue_present;

    FrameGenerationCreateDesc fg_desc = {};
    fg_desc.header.type = kTypeFrameGenerationCreate;
    fg_desc.flags = 0;
    fg_desc.displaySize = {create_info.imageExtent.width, create_info.imageExtent.height};
    fg_desc.maxRenderSize = fg_desc.displaySize;
    fg_desc.backBufferFormat = ffxApiGetSurfaceFormatVK(create_info.imageFormat);
    BackendDesc backend_desc = {};
    backend_desc.header.type = kTypeBackendVk;
    backend_desc.device_context = {device_->device(), device_->physical_device(),
                                   GetFfxDeviceProcAddr(device_)};
    VersionDesc fg_version_desc = {};
    fg_version_desc.header.type = kTypeFrameGenerationVersion;
    fg_version_desc.version = kFrameGenerationVersion;
    fg_desc.header.pNext = &backend_desc.header;
    backend_desc.header.pNext = &fg_version_desc.header;
    ffxContext fg_context = nullptr;
    const ffxReturnCode_t fg_created = ffx_.create_context(&fg_context, &fg_desc.header, nullptr);
    if (fg_created != FFX_API_RETURN_OK) {
      REXLOG_WARN("Frame generation: could not create the frame generation context: error {}",
                  fg_created);
      ReleaseSwapchain(swapchain);
      return false;
    }
    fg_context_ = fg_context;
    display_width_ = create_info.imageExtent.width;
    display_height_ = create_info.imageExtent.height;

    ffxQueryGetProviderVersion provider = {};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (ffx_.query(&fg_context, &provider.header) == FFX_API_RETURN_OK && provider.versionName) {
      version_ = provider.versionName;
    }
    REXLOG_INFO("Frame generation: FSR frame generation {} on a {}x{} swapchain", version_,
                display_width_, display_height_);
    return true;
  }

  void ReleaseSwapchain(VkSwapchainKHR& swapchain) override {
    if (!swapchain_context_) {
      return;
    }
    ffxContext swapchain_context = swapchain_context_;
    ffxDispatchDescHeader wait = {};
    wait.type = kTypeSwapchainWaitForPresents;
    ffx_.dispatch(&swapchain_context, &wait);
    if (fg_context_) {
      // Detach the swapchain from the context before it goes.
      FrameGenerationConfigureDesc detach = {};
      detach.header.type = kTypeFrameGenerationConfigure;
      detach.frameGenerationEnabled = false;
      detach.frameID = frame_id_;
      ffxContext fg_context = fg_context_;
      ffx_.configure(&fg_context, &detach.header);
      ffx_.destroy_context(&fg_context, nullptr);
      fg_context_ = nullptr;
    }
    ffx_.destroy_context(&swapchain_context, nullptr);
    swapchain_context_ = nullptr;
    if (destroy_swapchain_ && swapchain_ != VK_NULL_HANDLE) {
      destroy_swapchain_(device_->device(), swapchain_, nullptr, nullptr);
    }
    REXLOG_INFO("Frame generation: released the frame generation swapchain");
    swapchain_ = VK_NULL_HANDLE;
    swapchain = VK_NULL_HANDLE;
    destroy_swapchain_ = nullptr;
    functions_ = {};
    prepared_frame_id_ = UINT64_MAX;
    composing_ = false;
  }

  bool wraps_swapchain() const override { return swapchain_context_ != nullptr; }
  const SwapchainFunctions& swapchain_functions() const override { return functions_; }

  bool Prepare(VkCommandBuffer command_buffer, const VulkanUpscalerInputs& inputs) override {
    if (!fg_context_ || !inputs.depth.image || !inputs.motion_vectors.image) {
      return false;
    }
    const UpscalerFrame& frame = inputs.frame;
    FrameGenerationPrepareDesc desc = {};
    desc.header.type = kTypeFrameGenerationPrepareV2;
    desc.frameID = frame_id_;
    desc.flags = DebugFlags();
    desc.commandList = command_buffer;
    desc.renderSize = {frame.render_width, frame.render_height};
    desc.jitterOffset = {frame.jitter_x, frame.jitter_y};
    desc.motionVectorScale = {frame.motion_vector_scale_x, frame.motion_vector_scale_y};
    desc.frameTimeDelta = frame.frame_delta_ms;
    desc.reset = frame.reset;
    desc.cameraNear = frame.camera_near;
    desc.cameraFar = frame.camera_far;
    desc.cameraFovAngleVertical = frame.camera_fov_y;
    desc.viewSpaceToMetersFactor = 1.0f;
    desc.depth = Resource(inputs.depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    desc.motionVectors = Resource(inputs.motion_vectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    ffxContext fg_context = fg_context_;
    if (ffx_.dispatch(&fg_context, &desc.header) != FFX_API_RETURN_OK) {
      return false;
    }
    prepared_frame_id_ = frame_id_;
    return true;
  }

  void SetComposeCallback(ComposeCallback callback) override {
    compose_callback_ = std::move(callback);
  }

  void ConfigureForPresent(VkSwapchainKHR swapchain, bool generate, bool compose) override {
    if (!fg_context_) {
      return;
    }
    // The HUD layer goes over the rendered and the generated frame alike in
    // the presenter's own composition, as AMD's sample does with its UI (its
    // mode 2): the swapchain's composition of a registered UI resource reads
    // a null object in the driver here (RX 9060 XT, driver 32.0.32015.7029).
    compose = compose && compose_callback_;
    if (compose != composing_) {
      composing_ = compose;
      REXLOG_INFO("Frame generation: HUD {}", compose ? "as its own layer" : "in the frame");
    }
    FrameGenerationConfigureDesc desc = {};
    desc.header.type = kTypeFrameGenerationConfigure;
    desc.swapChain = reinterpret_cast<void*>(swapchain);
    if (compose) {
      desc.presentCallback = PresentCallback;
      desc.presentCallbackUserContext = this;
    }
    desc.frameGenerationCallback = DispatchFrameGeneration;
    desc.frameGenerationCallbackUserContext = &fg_context_;
    desc.frameGenerationEnabled = generate && prepared_frame_id_ == frame_id_;
    desc.allowAsyncWorkloads = false;
    desc.flags = DebugFlags();
    desc.onlyPresentGenerated = false;
    desc.generationRect = {0, 0, int32_t(display_width_), int32_t(display_height_)};
    desc.frameID = frame_id_;
    ffxContext fg_context = fg_context_;
    const ffxReturnCode_t configured = ffx_.configure(&fg_context, &desc.header);
    if (configured != FFX_API_RETURN_OK && !configure_failure_logged_) {
      configure_failure_logged_ = true;
      REXLOG_WARN("Frame generation: configuration failed: error {}", configured);
    }
    if (desc.frameGenerationEnabled != last_enabled_) {
      last_enabled_ = desc.frameGenerationEnabled;
      REXLOG_INFO("Frame generation: {}", last_enabled_ ? "generating" : "paused");
    }
    ++frame_id_;
  }

  const char* version() const override { return version_.c_str(); }

 private:
  static ffxReturnCode_t PresentCallback(PresentCallbackDesc* params, void* user_context) {
    auto* self = static_cast<VulkanFfxFrameGeneration*>(user_context);
    if (!self || !self->compose_callback_ || !params->commandList) {
      return FFX_API_RETURN_ERROR_PARAMETER;
    }
    const bool composed = self->compose_callback_(
        static_cast<VkCommandBuffer>(params->commandList), PresentImageOf(params->currentBackBuffer),
        PresentImageOf(params->outputSwapChainBuffer));
    return composed ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR;
  }

  static uint32_t DebugFlags() {
    return REXCVAR_GET(present_frame_generation_debug)
               ? kFlagDrawDebugTearLines | kFlagDrawDebugResetIndicators | kFlagDrawDebugPacingLines
               : 0;
  }

  const VulkanDevice* device_;
  FfxApiEntryPoints ffx_;
  ffxContext swapchain_context_ = nullptr;
  ffxContext fg_context_ = nullptr;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  PfnDestroySwapchainFfx destroy_swapchain_ = nullptr;
  SwapchainFunctions functions_;
  uint32_t display_width_ = 0;
  uint32_t display_height_ = 0;
  uint64_t frame_id_ = 1;
  uint64_t prepared_frame_id_ = UINT64_MAX;
  bool last_enabled_ = false;
  bool configure_failure_logged_ = false;
  ComposeCallback compose_callback_;
  bool composing_ = false;
  std::string version_;
};

}  // namespace
#endif  // defined(REX_HAS_FFX_API) && REX_HAS_FFX_API

std::unique_ptr<VulkanFrameGeneration> VulkanFrameGeneration::Create(const VulkanDevice* device,
                                                                     const char** reason_out) {
  const char* unused_reason;
  const char*& reason = reason_out ? *reason_out : unused_reason;
#if defined(REX_HAS_FFX_API) && REX_HAS_FFX_API
  const FfxApiEntryPoints ffx = GetFfxApiEntryPoints();
  if (!ffx.loaded()) {
    reason = "the FidelityFX runtime is unavailable";
    return nullptr;
  }
  if (!ffx.configure) {
    reason = "the FidelityFX runtime is older than SDK 2.x (no ffxConfigure)";
    return nullptr;
  }
  const VulkanDevice::FrameGenerationQueues& queues = device->frame_generation_queues();
  if (!queues.present.valid() || !queues.async_compute.valid() || !queues.image_acquire.valid()) {
    reason = "the GPU has no spare queues for the frame generation swapchain";
    return nullptr;
  }
  DeviceContext device_context = {device->device(), device->physical_device(),
                                  GetFfxDeviceProcAddr(device)};
  for (uint64_t create_type : {kTypeSwapchainCreate, kTypeFrameGenerationCreate}) {
    uint64_t count = 0;
    ffxQueryDescGetVersions query = {};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = create_type;
    query.device = &device_context;
    query.outputCount = &count;
    if (ffx.query(nullptr, &query.header) != FFX_API_RETURN_OK || !count) {
      reason = "the FidelityFX runtime has no frame generation "
               "(amd_fidelityfx_framegeneration_vk.dll is missing)";
      return nullptr;
    }
  }
  reason = nullptr;
  return std::make_unique<VulkanFfxFrameGeneration>(device, ffx);
#else
  (void)device;
  reason = "built without the FidelityFX API";
  return nullptr;
#endif
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
