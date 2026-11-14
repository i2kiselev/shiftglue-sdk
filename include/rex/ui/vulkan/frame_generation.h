/**
 * AMD FidelityFX frame generation on the Vulkan presenter (FidelityFX SDK 2.4
 * runtime: amd_fidelityfx_framegeneration_vk.dll beside the loader).
 *
 * AMD's frame generation swapchain replaces the presenter's VkSwapchainKHR:
 * the presenter acquires and presents through the replacement functions, and
 * on each present the swapchain presents an interpolated frame before the
 * rendered one, pacing both itself. The presenter records the per-frame
 * preparation (depth, motion vectors, camera) beside the temporal upscaler.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/upscaler.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanDevice;

class VulkanFrameGeneration {
 public:
  // An image the swapchain hands the present callback, in `layout`, which it
  // must be left in.
  struct PresentImage {
    VkImage image = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };
  // Records into `command_buffer` (run on the swapchain's present queue,
  // possibly from its own thread) the composition of `back_buffer`, rendered
  // or generated, into `output`; false if nothing was recorded.
  using ComposeCallback = std::function<bool(VkCommandBuffer command_buffer,
                                             const PresentImage& back_buffer,
                                             const PresentImage& output)>;

  struct SwapchainFunctions {
    PFN_vkGetSwapchainImagesKHR get_swapchain_images = nullptr;
    PFN_vkAcquireNextImageKHR acquire_next_image = nullptr;
    PFN_vkQueuePresentKHR queue_present = nullptr;
  };

  // Null when the runtime, its frame generation or the spare queues are
  // missing; `reason_out` then says why.
  static std::unique_ptr<VulkanFrameGeneration> Create(const VulkanDevice* device,
                                                       const char** reason_out);
  virtual ~VulkanFrameGeneration() = default;

  // Replaces `swapchain`, created from `create_info` and presented on
  // `game_queue_family` queue 0, with AMD's. On failure `swapchain` may have
  // been destroyed and is nulled.
  virtual bool WrapSwapchain(VkSwapchainKHR& swapchain,
                             const VkSwapchainCreateInfoKHR& create_info,
                             uint32_t game_queue_family) = 0;
  // Waits for the presents in flight, destroys the contexts and the wrapped
  // swapchain, and nulls `swapchain`. No-op when nothing is wrapped.
  virtual void ReleaseSwapchain(VkSwapchainKHR& swapchain) = 0;
  virtual bool wraps_swapchain() const = 0;
  // Valid while a swapchain is wrapped.
  virtual const SwapchainFunctions& swapchain_functions() const = 0;

  // Records this frame's preparation from the temporal upscaler's inputs;
  // false if nothing was recorded (the frame is then presented without an
  // interpolated one).
  virtual bool Prepare(VkCommandBuffer command_buffer, const VulkanUpscalerInputs& inputs) = 0;
  // The composition of presented frames while ConfigureForPresent asks for
  // it, in place of the swapchain's own.
  virtual void SetComposeCallback(ComposeCallback callback) = 0;
  // Before presenting: generates a frame before this one when `generate` and
  // this frame was prepared; with `compose`, the compose callback composes
  // every presented frame (the HUD layer over it). Call once per present.
  virtual void ConfigureForPresent(VkSwapchainKHR swapchain, bool generate, bool compose) = 0;
  // Version name of the frame generation provider in use, empty before one.
  virtual const char* version() const = 0;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
