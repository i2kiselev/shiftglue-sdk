#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include <rex/cvar.h>
#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/upscaler.h>

REXCVAR_DECLARE(int32_t, present_frame_generation_multiplier);
REXCVAR_DECLARE(std::string, present_frame_generation);
REXCVAR_DECLARE(bool, present_fg_timing);

namespace rex::ui::vulkan {

class VulkanDevice;
class VulkanFsrFrameGeneration;

// Owns generated frame images and selects the requested interpolation method.
// A provider is only reported available after it exposes a real FG feature.
class FrameGeneration final {
 public:
  static constexpr uint32_t kMaxImages = 3;

  FrameGeneration(const VulkanDevice* device, VulkanUpscaler* dlss_provider);
  ~FrameGeneration();

  FrameGeneration(const FrameGeneration&) = delete;
  FrameGeneration& operator=(const FrameGeneration&) = delete;

  bool Configure(uint32_t width, uint32_t height, VkFormat format);
  bool Available() const;
  uint32_t MaxGenerated() const;
  uint32_t Record(VkCommandBuffer command_buffer, const VulkanFrameGenerationInputs& inputs,
                  uint32_t requested_factor);
  void MarkPresented(bool presented);
  void Retire(uint64_t completed_frame_id);
  std::span<const VulkanUpscalerImage> images() const { return images_; }
  void Shutdown();

 private:
  bool EnsureImages(uint32_t width, uint32_t height, VkFormat format);

  const VulkanDevice* device_;
  VulkanUpscaler* dlss_provider_ = nullptr;
  std::unique_ptr<VulkanFsrFrameGeneration> fsr_provider_;
  std::array<VulkanUpscalerImage, kMaxImages> images_{};
  std::array<VkDeviceMemory, kMaxImages> image_memories_{};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
};

}  // namespace rex::ui::vulkan
