#pragma once
/**
 * Vulkan temporal upscaler providers (rex/ui/upscaler.h).
 */

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <rex/ui/upscaler.h>
#include <rex/ui/vulkan/api.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;

struct VulkanUpscalerImage {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
  // Images that were created with storage usage (the output must be).
  VkImageUsageFlags usage = 0;
};

// Inputs are in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, readable by compute
// shaders; the output is in VK_IMAGE_LAYOUT_GENERAL, writable by them. Missing
// optional inputs have a null image.
struct VulkanUpscalerInputs {
  VulkanUpscalerImage color;
  // R32_SFLOAT or a depth format, 0 near and 1 far unless depth_inverted.
  VulkanUpscalerImage depth;
  // RG16_SFLOAT, in render pixels after motion_vector_scale.
  VulkanUpscalerImage motion_vectors;
  VulkanUpscalerImage reactive_mask;
  VulkanUpscalerImage transparency_mask;
  VulkanUpscalerImage output;
  UpscalerFrame frame;
};

struct VulkanFrameGenerationInputs {
  // Colour at the output size, HUD-free and in GENERAL layout.
  VulkanUpscalerImage color;
  // Render-resolution depth and current-to-previous motion in normalized UV.
  // Each provider scales UV motion by the render dimensions it passes to its SDK.
  VulkanUpscalerImage depth;
  VulkanUpscalerImage motion_vectors;
  UpscalerCamera camera;
  float frame_delta_ms = 16.667f;
  bool reset = true;
  uint64_t render_frame_index = 0;
};

class VulkanUpscaler {
 public:
  virtual ~VulkanUpscaler() = default;

  virtual const UpscalerInfo& info() const = 0;
  // Creates the provider's resources for frames up to these sizes, or keeps
  // them if they already fit. The previous resources must be out of use by
  // the GPU. False if the provider cannot run.
  virtual bool Prepare(uint32_t max_render_width, uint32_t max_render_height,
                       uint32_t max_output_width, uint32_t max_output_height, bool hdr) = 0;
  // Records the upscale; false if nothing was recorded.
  virtual bool Dispatch(VkCommandBuffer command_buffer, const VulkanUpscalerInputs& inputs) = 0;
  // Frees the resources Prepare made (the GPU must be done with them).
  virtual void Release() = 0;

  // Optional frame-interpolation path. Generated outputs are in GENERAL layout
  // and remain owned by the caller until its GPU and presentation work ends.
  virtual bool FrameGenerationAvailable() const { return false; }
  virtual uint32_t FrameGenerationMaxGenerated() const { return 0; }
  virtual bool PrepareFrameGeneration(uint32_t output_width, uint32_t output_height,
                                      VkFormat output_format) {
    return false;
  }
  virtual uint32_t DispatchFrameGeneration(
      VkCommandBuffer command_buffer, const VulkanFrameGenerationInputs& inputs,
      std::span<VulkanUpscalerImage> outputs, uint32_t requested_factor) {
    return 0;
  }
};

// The temporal providers built into this build, available or not.
std::vector<std::unique_ptr<VulkanUpscaler>> CreateVulkanUpscalers(const VulkanDevice* device);

// DLSS through NGX, or null when the build has no DLSS (REX_HAS_DLSS).
std::unique_ptr<VulkanUpscaler> CreateVulkanDlssUpscaler(const VulkanDevice* device);

// Extensions the providers need on the instance, and on a device of this
// vendor (NGX for DLSS). Instance and device creation enable those that are
// supported. The names stay valid for the life of the process.
std::vector<const char*> GetVulkanUpscalerInstanceExtensions();
std::vector<const char*> GetVulkanUpscalerDeviceExtensions(VkInstance instance,
                                                           VkPhysicalDevice physical_device,
                                                           uint32_t vendor_id);

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
