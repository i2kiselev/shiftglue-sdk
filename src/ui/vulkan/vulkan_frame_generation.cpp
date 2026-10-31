// SPDX-License-Identifier: GPL-2.0-or-later
// Frame-resource orchestration adapted from bbport's paced frame-generation
// integration. The selected interpolation providers remain separate.

#include <rex/ui/vulkan/frame_generation.h>

#include <algorithm>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/frame_generation_status.h>
#include <rex/ui/vulkan/util.h>

#if defined(REX_HAS_FSR_FRAMEGENERATION) && REX_HAS_FSR_FRAMEGENERATION
#include "ui/vulkan/vulkan_fsr_frame_generation.h"
#endif

REXCVAR_DEFINE_STRING(present_frame_generation, "off", "UI/Presenter",
                      "Frame generation backend: off, dlss, or fsr")
    .allowed({"off", "dlss", "fsr"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(present_frame_generation_multiplier, 2, "UI/Presenter",
                     "Frame-generation multiplier; the native presenter supports 2x")
    .range(2, 2)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(present_fg_timing, false, "UI/Presenter",
                    "Log source, generated and skipped presentation cadence every 120 source frames");

namespace rex::ui::vulkan {
namespace {

std::mutex g_status_mutex;
std::string g_frame_generation_status = "Frame generation: not initialized";

void SetStatus(const std::string& status) {
  {
    std::lock_guard lock(g_status_mutex);
    if (g_frame_generation_status == status) return;
    g_frame_generation_status = status;
  }
  REXLOG_INFO("{}", status);
}

}  // namespace

FrameGeneration::FrameGeneration(const VulkanDevice* device, VulkanUpscaler* dlss_provider)
    : device_(device), dlss_provider_(dlss_provider) {
#if defined(REX_HAS_FSR_FRAMEGENERATION) && REX_HAS_FSR_FRAMEGENERATION
  if (REXCVAR_GET(present_frame_generation) == "fsr" && device_) {
    fsr_provider_ = std::make_unique<VulkanFsrFrameGeneration>(*device_);
  }
#endif
  const std::string requested = REXCVAR_GET(present_frame_generation);
  if (requested == "off") {
    SetStatus("Frame generation: off");
  } else if (requested == "dlss") {
    if (!dlss_provider_) {
      SetStatus("Frame generation: DLSS unavailable (provider was not built)");
    } else if (!dlss_provider_->FrameGenerationAvailable()) {
      SetStatus("Frame generation: DLSS unavailable (runtime/GPU capability check failed)");
    } else {
      SetStatus("Frame generation: DLSS available, waiting for the first frame");
    }
  } else {
#if defined(REX_HAS_FSR_FRAMEGENERATION) && REX_HAS_FSR_FRAMEGENERATION
    SetStatus(Fm4FsrFrameGenerationStatus());
#else
    SetStatus("Frame generation: FSR unavailable (backend was not built)");
#endif
  }
}

FrameGeneration::~FrameGeneration() {
  Shutdown();
}

bool FrameGeneration::Available() const {
  const std::string requested = REXCVAR_GET(present_frame_generation);
  if (requested == "dlss") return dlss_provider_ && dlss_provider_->FrameGenerationAvailable();
  if (requested == "fsr") return fsr_provider_ != nullptr;
  return false;
}

uint32_t FrameGeneration::MaxGenerated() const {
  if (!Available()) return 0;
  if (REXCVAR_GET(present_frame_generation) == "fsr") return 1;
  // The first production integration presents one interpolated frame per
  // source. NGX may advertise more, but 2x is the honest supported path until
  // every output can have dedicated HUD composition descriptors.
  return std::min({dlss_provider_->FrameGenerationMaxGenerated(), 1u, kMaxImages});
}

bool FrameGeneration::Configure(uint32_t width, uint32_t height, VkFormat format) {
  if (!Available()) return false;
  if (!EnsureImages(width, height, format)) {
    SetStatus("Frame generation: unavailable (output image preparation failed)");
    return false;
  }
#if defined(REX_HAS_FSR_FRAMEGENERATION) && REX_HAS_FSR_FRAMEGENERATION
  if (REXCVAR_GET(present_frame_generation) == "fsr") {
    if (!fsr_provider_->Prepare(width, height, format)) {
      SetStatus(Fm4FsrFrameGenerationStatus());
      return false;
    }
    return true;
  }
#endif
  if (!dlss_provider_->PrepareFrameGeneration(width, height, format)) {
    SetStatus("Frame generation: DLSS unavailable (feature preparation failed)");
    return false;
  }
  return true;
}

uint32_t FrameGeneration::Record(VkCommandBuffer command_buffer,
                                 const VulkanFrameGenerationInputs& inputs,
                                 uint32_t requested_factor) {
  if (!Available() || !width_ || !height_) return 0;
  const uint32_t count =
      std::min({requested_factor - 1u, MaxGenerated(), static_cast<uint32_t>(images_.size())});
  if (!count) return 0;
  uint32_t recorded = 0;
#if defined(REX_HAS_FSR_FRAMEGENERATION) && REX_HAS_FSR_FRAMEGENERATION
  if (REXCVAR_GET(present_frame_generation) == "fsr") {
    if (fsr_provider_->Record(command_buffer, inputs, images_[0],
                              inputs.render_frame_index, inputs.reset)) {
      recorded = inputs.reset ? 0 : 1;
    }
  } else
#endif
  {
    recorded = dlss_provider_->DispatchFrameGeneration(
        command_buffer, inputs, std::span<VulkanUpscalerImage>(images_.data(), count),
        requested_factor);
  }
  return recorded;
}

void FrameGeneration::Retire(uint64_t completed_frame_id) {
#if defined(REX_HAS_FSR_FRAMEGENERATION) && REX_HAS_FSR_FRAMEGENERATION
  if (fsr_provider_) fsr_provider_->Retire(completed_frame_id);
#else
  (void)completed_frame_id;
#endif
}

void FrameGeneration::MarkPresented(bool presented) {
  const std::string backend =
      REXCVAR_GET(present_frame_generation) == "fsr" ? "FSR" : "DLSS";
  SetStatus(std::string("Frame generation: ") + backend +
            (presented ? " presenting 2x" : " generating; midpoint skipped"));
}

bool FrameGeneration::EnsureImages(uint32_t width, uint32_t height, VkFormat format) {
  if (images_[0].image && width_ == width && height_ == height && format_ == format) {
    return true;
  }
  Shutdown();
  VkFormatProperties properties{};
  device_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
      device_->physical_device(), format, &properties);
  if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
      !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
    return false;
  }

  const VulkanDevice::Functions& functions = device_->functions();
  const VkDevice device = device_->device();
  for (uint32_t i = 0; i < kMaxImages; ++i) {
    VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (!util::CreateDedicatedAllocationImage(device_, image_info,
                                              util::MemoryPurpose::kDeviceLocal, image, memory)) {
      Shutdown();
      return false;
    }
    VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (functions.vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
      functions.vkDestroyImage(device, image, nullptr);
      functions.vkFreeMemory(device, memory, nullptr);
      Shutdown();
      return false;
    }
    images_[i] = {image, view, format, width, height, image_info.usage};
    image_memories_[i] = memory;
  }
  width_ = width;
  height_ = height;
  format_ = format;
  return true;
}

void FrameGeneration::Shutdown() {
  if (!device_) {
    images_ = {};
    image_memories_ = {};
    return;
  }
  const VulkanDevice::Functions& functions = device_->functions();
  const VkDevice device = device_->device();
  for (uint32_t i = 0; i < kMaxImages; ++i) {
    if (images_[i].view) functions.vkDestroyImageView(device, images_[i].view, nullptr);
    if (images_[i].image) functions.vkDestroyImage(device, images_[i].image, nullptr);
    if (image_memories_[i]) functions.vkFreeMemory(device, image_memories_[i], nullptr);
  }
  images_ = {};
  image_memories_ = {};
  width_ = height_ = 0;
  format_ = VK_FORMAT_UNDEFINED;
}

std::string Fm4FrameGenerationStatus() {
  std::lock_guard lock(g_status_mutex);
  return g_frame_generation_status;
}

}  // namespace rex::ui::vulkan
