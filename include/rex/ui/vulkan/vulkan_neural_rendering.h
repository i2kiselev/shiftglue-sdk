#pragma once
/**
 ******************************************************************************
 * ReXGlue native DLSS Neural Rendering bridge.
 *
 * The model itself lives in NVIDIA's runtime. This interface owns the native
 * Vulkan lifetime, the render-resolution FP16 proxies, and the pre-temporal-
 * upscale conversion back to the guest output format.
 ******************************************************************************
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/upscaler.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;

// Instance and device extensions required by the optional Neural Rendering
// bridge. They are empty when the feature is disabled, the GPU is not NVIDIA,
// or the bridge/runtime is not installed. The returned names remain valid for
// the lifetime of the process.
std::vector<const char*> GetVulkanNeuralRenderingInstanceExtensions();
std::vector<const char*> GetVulkanNeuralRenderingDeviceExtensions(VkInstance instance,
                                                                  VkPhysicalDevice physical_device,
                                                                  uint32_t vendor_id);

// VK_KHR_buffer_device_address is required by the model. VulkanDevice enables
// one bufferDeviceAddress feature bit for all optional providers that ask.
bool VulkanNeuralRenderingRequiresBufferDeviceAddress();

// Whether the bridge and the user-provided NR model runtime are installed.
// This does not initialize the model or require a Vulkan device.
bool IsVulkanNeuralRenderingInstalled();

// Last model state for the graphics page, synchronized internally.
std::string GetVulkanNeuralRenderingStatus();

class VulkanNeuralRendering final {
 public:
  explicit VulkanNeuralRendering(const VulkanDevice* device);
  ~VulkanNeuralRendering();

  VulkanNeuralRendering(const VulkanNeuralRendering&) = delete;
  VulkanNeuralRendering& operator=(const VulkanNeuralRendering&) = delete;

  bool initialized() const;

  // True when changing settings or image size would release model or image
  // objects. The presenter must wait for its previous paint submission before
  // calling Dispatch in this case.
  bool RequiresGpuWait(uint32_t width, uint32_t height) const;

  // Records a complete pre-upscale NR pass into command_buffer. The bridge
  // image lifetime contract is handled here. When the model has not finished
  // uploading, returns false and the caller uses its original source for that
  // frame. processed is the A2B10G10R10, shader-readable staging input.
  bool Dispatch(VkCommandBuffer command_buffer, uint32_t descriptor_set_index,
                const VulkanUpscalerImage& color, const VulkanUpscalerImage& motion_vectors,
                const VulkanUpscalerImage& depth, bool depth_inverted, bool reset,
                float motion_scale_x, float motion_scale_y, VulkanUpscalerImage& processed);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
