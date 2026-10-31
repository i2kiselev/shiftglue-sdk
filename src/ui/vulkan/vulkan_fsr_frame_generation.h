#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/upscaler.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;

// Owns the FSR 3.1.6 frame-generation context only. Input and output images
// remain owned by the caller, normally the presenter or its guest-output flow.
//
// color/output: A2B10G10R10_UNORM, RGBA8_UNORM or RGBA16_SFLOAT at display
// size. Inputs require TRANSFER_SRC and output requires TRANSFER_DST. The
// provider temporarily converts to its own RGBA16 images. depth: R32_SFLOAT at
// render size. motion_vectors: RG16_SFLOAT or RGBA16_SFLOAT at render size and
// normalized UV (the provider scales by the render extent). Inputs and output
// are in GENERAL layout; the provider restores them to GENERAL after use.
class VulkanFsrFrameGeneration final {
 public:
  static constexpr uint32_t kMaxGeneratedFrames = 1;

  using Image = VulkanUpscalerImage;
  using Input = VulkanFrameGenerationInputs;
  using Output = VulkanUpscalerImage;

  explicit VulkanFsrFrameGeneration(const VulkanDevice& device);
  ~VulkanFsrFrameGeneration();

  VulkanFsrFrameGeneration(const VulkanFsrFrameGeneration&) = delete;
  VulkanFsrFrameGeneration& operator=(const VulkanFsrFrameGeneration&) = delete;
  VulkanFsrFrameGeneration(VulkanFsrFrameGeneration&&) = delete;
  VulkanFsrFrameGeneration& operator=(VulkanFsrFrameGeneration&&) = delete;

  // Recreates the provider when display extent or color format changes. All
  // previously recorded frame IDs must be retired before changing either.
  bool Prepare(uint32_t width, uint32_t height, VkFormat format);

  // Records prepare and dispatch into command_buffer. frame_id must be
  // monotonically increasing. A false backend result may leave partial work
  // in the command buffer, so retire frame_id after that work is complete.
  bool Record(VkCommandBuffer command_buffer, const Input& input, const Output& output,
              uint64_t frame_id, bool reset);
  // Generic-interface form, using input.render_frame_index and input.reset.
  bool Record(VkCommandBuffer command_buffer, const Input& input, const Output& output);

  // Call after the submission containing frame_id has completed on the GPU.
  void Retire(uint64_t completed_frame_id);

  // True only after the real provider context has been created successfully.
  [[nodiscard]] bool Available() const;

  // The caller must ensure the device no longer uses the provider, records,
  // or borrowed images before shutdown.
  void Shutdown();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Deliberately distinct from the main-owned Fm4FrameGenerationStatus symbol.
std::string Fm4FsrFrameGenerationStatus();

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
