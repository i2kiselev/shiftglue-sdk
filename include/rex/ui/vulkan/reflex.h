// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/ui/vulkan/api.h>

REXCVAR_DECLARE(std::string, present_reflex);

namespace rex::ui::vulkan {

class VulkanDevice;

// VK_NV_low_latency2 provider for the renderer's active swapchain.
class Reflex final {
 public:
  explicit Reflex(const VulkanDevice* device);
  ~Reflex();

  Reflex(const Reflex&) = delete;
  Reflex& operator=(const Reflex&) = delete;

  void OnSwapchainCreated(VkSwapchainKHR swapchain);
  void OnSwapchainDestroying(VkSwapchainKHR swapchain);

  bool active() const;
  bool IsAvailable() const;
  uint64_t BeginFrame();
  void InputSampled(uint64_t frame_id);
  void SimulationStart(uint64_t frame_id);
  void SimulationEnd(uint64_t frame_id);
  void RenderSubmitStart(uint64_t frame_id);
  void RenderSubmitEnd(uint64_t frame_id);
  void PresentStart(uint64_t frame_id);
  void PresentEnd(uint64_t frame_id);
  void OutOfBandPresentStart(uint64_t frame_id);
  void OutOfBandPresentEnd(uint64_t frame_id);
  bool Pace(uint64_t frame_id);

 private:
  void SetMarker(uint64_t frame_id, VkLatencyMarkerNV marker);
  void SetActive(bool active, const char* reason);

  const VulkanDevice* device_;
  mutable std::mutex mutex_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkSemaphore sleep_semaphore_ = VK_NULL_HANDLE;
  PFN_vkSetLatencySleepModeNV set_sleep_mode_ = nullptr;
  PFN_vkLatencySleepNV latency_sleep_ = nullptr;
  PFN_vkSetLatencyMarkerNV set_marker_ = nullptr;
  PFN_vkWaitSemaphores wait_semaphores_ = nullptr;
  bool available_ = false;
  bool active_ = false;
  bool boost_ = false;
  uint64_t next_frame_id_ = 0;
};

// Real guest input and simulation boundary hooks. These are safe to call when
// Reflex is off or unavailable.
void Fm4ReflexBeforeInputSample();
void Fm4ReflexAfterInputSample();
void Fm4ReflexSimulationEnd();
uint64_t Fm4ReflexCurrentFrameId();

}  // namespace rex::ui::vulkan
