// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from bbport's VK_NV_low_latency2 integration and its marker cadence.

#include <rex/ui/vulkan/reflex.h>

#include <atomic>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/frame_generation_status.h>
#include <rex/ui/vulkan/instance.h>

REXCVAR_DEFINE_STRING(present_reflex, "on", "UI/Presenter",
                      "NVIDIA Reflex when VK_NV_low_latency2 is available: off, on or boost")
    .allowed({"off", "on", "boost"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::ui::vulkan {
namespace {

std::mutex g_status_mutex;
std::string g_reflex_status = "Reflex: not initialized";
std::mutex g_live_reflex_mutex;
Reflex* g_live_reflex = nullptr;
thread_local uint64_t tls_input_frame_id = 0;
thread_local bool tls_input_sample_marked = false;
std::atomic<uint64_t> g_current_frame_id = 0;
std::atomic<bool> g_input_frame_open = false;

void PublishStatus(const std::string& status) {
  {
    std::lock_guard lock(g_status_mutex);
    g_reflex_status = status;
  }
  REXLOG_INFO("{}", status);
}

}  // namespace

Reflex::Reflex(const VulkanDevice* device) : device_(device) {
  {
    std::lock_guard lock(g_live_reflex_mutex);
    g_live_reflex = this;
  }
  const std::string requested = REXCVAR_GET(present_reflex);
  boost_ = requested == "boost";
  if (requested == "off") {
    PublishStatus("Reflex: off");
    return;
  }
  if (!device || !device->extensions().ext_NV_low_latency2) {
    PublishStatus("Reflex: unavailable (VK_NV_low_latency2 was not enabled)");
    return;
  }

  const VulkanInstance* instance = device->vulkan_instance();
  const PFN_vkGetInstanceProcAddr get_instance_proc = instance->functions().vkGetInstanceProcAddr;
  const auto get_device_proc =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance_proc(instance->instance(),
                                                                  "vkGetDeviceProcAddr"));
  if (!get_device_proc) {
    PublishStatus("Reflex: unavailable (vkGetDeviceProcAddr is missing)");
    return;
  }
  set_sleep_mode_ = reinterpret_cast<PFN_vkSetLatencySleepModeNV>(
      get_device_proc(device_->device(), "vkSetLatencySleepModeNV"));
  latency_sleep_ = reinterpret_cast<PFN_vkLatencySleepNV>(
      get_device_proc(device_->device(), "vkLatencySleepNV"));
  set_marker_ = reinterpret_cast<PFN_vkSetLatencyMarkerNV>(
      get_device_proc(device_->device(), "vkSetLatencyMarkerNV"));
  wait_semaphores_ = reinterpret_cast<PFN_vkWaitSemaphores>(
      get_device_proc(device_->device(), "vkWaitSemaphores"));
  if (!set_sleep_mode_ || !latency_sleep_ || !set_marker_ || !wait_semaphores_) {
    PublishStatus("Reflex: unavailable (the NVIDIA driver did not export latency2 entry points)");
    return;
  }
  if (!device->properties().presentId || !device->properties().timelineSemaphore) {
    PublishStatus("Reflex: unavailable (present IDs or timeline semaphores are disabled)");
    return;
  }

  VkSemaphoreTypeCreateInfo type_info = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  semaphore_info.pNext = &type_info;
  if (device->functions().vkCreateSemaphore(device->device(), &semaphore_info, nullptr,
                                            &sleep_semaphore_) != VK_SUCCESS) {
    PublishStatus("Reflex: unavailable (could not create the pacing semaphore)");
    return;
  }

  available_ = true;
  PublishStatus("Reflex: available, waiting for the swapchain");
}

Reflex::~Reflex() {
  {
    std::lock_guard lock(g_live_reflex_mutex);
    if (g_live_reflex == this) g_live_reflex = nullptr;
  }
  OnSwapchainDestroying(swapchain_);
  if (sleep_semaphore_ != VK_NULL_HANDLE) {
    device_->functions().vkDestroySemaphore(device_->device(), sleep_semaphore_, nullptr);
  }
}

void Reflex::SetActive(bool active, const char* reason) {
  {
    std::lock_guard lock(mutex_);
    active_ = active;
  }
  PublishStatus(std::string("Reflex: ") + reason);
}

bool Reflex::active() const {
  std::lock_guard lock(mutex_);
  return active_;
}

bool Reflex::IsAvailable() const {
  std::lock_guard lock(mutex_);
  return available_;
}

void Reflex::OnSwapchainCreated(VkSwapchainKHR swapchain) {
  bool available = false;
  {
    std::lock_guard lock(mutex_);
    swapchain_ = swapchain;
    available = available_;
  }
  if (!available) {
    return;
  }
  if (swapchain == VK_NULL_HANDLE) {
    SetActive(false, "available, waiting for the swapchain");
    return;
  }
  VkLatencySleepModeInfoNV info = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
  info.lowLatencyMode = VK_TRUE;
  info.lowLatencyBoost = boost_ ? VK_TRUE : VK_FALSE;
  const VkResult result = set_sleep_mode_(device_->device(), swapchain, &info);
  if (result != VK_SUCCESS) {
    const std::string reason =
        "unavailable (vkSetLatencySleepModeNV failed: " + std::to_string(int32_t(result)) + ")";
    SetActive(false, reason.c_str());
    return;
  }
  SetActive(true, boost_ ? "active (boost)" : "active");
}

void Reflex::OnSwapchainDestroying(VkSwapchainKHR swapchain) {
  std::lock_guard lock(mutex_);
  if (available_ && set_sleep_mode_ && swapchain != VK_NULL_HANDLE) {
    VkLatencySleepModeInfoNV info = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
    info.lowLatencyMode = VK_FALSE;
    set_sleep_mode_(device_->device(), swapchain, &info);
  }
  active_ = false;
  if (swapchain_ == swapchain) {
    swapchain_ = VK_NULL_HANDLE;
  }
}

uint64_t Reflex::BeginFrame() {
  return ++next_frame_id_;
}

void Reflex::InputSampled(uint64_t frame_id) {
  SetMarker(frame_id, VK_LATENCY_MARKER_INPUT_SAMPLE_NV);
}

void Reflex::SimulationStart(uint64_t frame_id) {
  SetMarker(frame_id, VK_LATENCY_MARKER_SIMULATION_START_NV);
}

void Reflex::SimulationEnd(uint64_t frame_id) {
  SetMarker(frame_id, VK_LATENCY_MARKER_SIMULATION_END_NV);
}

void Reflex::RenderSubmitStart(uint64_t frame_id) {
  SetMarker(frame_id, VK_LATENCY_MARKER_RENDERSUBMIT_START_NV);
}

void Reflex::RenderSubmitEnd(uint64_t frame_id) {
  SetMarker(frame_id, VK_LATENCY_MARKER_RENDERSUBMIT_END_NV);
}

void Reflex::PresentStart(uint64_t frame_id) {
  SetMarker(frame_id, VK_LATENCY_MARKER_PRESENT_START_NV);
}

void Reflex::PresentEnd(uint64_t frame_id) {
  SetMarker(frame_id, VK_LATENCY_MARKER_PRESENT_END_NV);
}

void Reflex::OutOfBandPresentStart(uint64_t frame_id) {
  SetMarker(frame_id, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV);
}

void Reflex::OutOfBandPresentEnd(uint64_t frame_id) {
  SetMarker(frame_id, VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV);
}

bool Reflex::Pace(uint64_t frame_id) {
  std::lock_guard lock(mutex_);
  if (!active_ || !frame_id || swapchain_ == VK_NULL_HANDLE || sleep_semaphore_ == VK_NULL_HANDLE) {
    return false;
  }
  VkLatencySleepInfoNV info = {VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV};
  info.signalSemaphore = sleep_semaphore_;
  info.value = frame_id;
  VkResult result = latency_sleep_(device_->device(), swapchain_, &info);
  if (result == VK_SUCCESS) {
    VkSemaphoreWaitInfo wait_info = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &sleep_semaphore_;
    wait_info.pValues = &frame_id;
    result = wait_semaphores_(device_->device(), &wait_info, UINT64_C(100000000));
  }
  if (result != VK_SUCCESS) {
    active_ = false;
    PublishStatus("Reflex: unavailable (NVIDIA latency pacing failed)");
    return false;
  }
  return true;
}

void Reflex::SetMarker(uint64_t frame_id, VkLatencyMarkerNV marker) {
  std::lock_guard lock(mutex_);
  if (!active_ || swapchain_ == VK_NULL_HANDLE || !set_marker_ || !frame_id) {
    return;
  }
  VkSetLatencyMarkerInfoNV info = {VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
  info.presentID = frame_id;
  info.marker = marker;
  set_marker_(device_->device(), swapchain_, &info);
}

std::string Fm4ReflexStatus() {
  std::lock_guard lock(g_status_mutex);
  return g_reflex_status;
}

void Fm4ReflexBeforeInputSample() {
  std::lock_guard live_lock(g_live_reflex_mutex);
  Reflex* reflex = g_live_reflex;
  if (g_input_frame_open.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  tls_input_frame_id = 0;
  tls_input_sample_marked = false;
  if (!reflex || !reflex->active()) {
    g_input_frame_open.store(false, std::memory_order_release);
    return;
  }
  const uint64_t frame_id = reflex->BeginFrame();
  if (reflex->Pace(frame_id)) {
    tls_input_frame_id = frame_id;
    g_current_frame_id.store(tls_input_frame_id, std::memory_order_release);
  }
}

void Fm4ReflexAfterInputSample() {
  std::lock_guard live_lock(g_live_reflex_mutex);
  Reflex* reflex = g_live_reflex;
  if (reflex && tls_input_frame_id && !tls_input_sample_marked) {
    reflex->InputSampled(tls_input_frame_id);
    reflex->SimulationStart(tls_input_frame_id);
    tls_input_sample_marked = true;
  }
}

void Fm4ReflexSimulationEnd() {
  std::lock_guard live_lock(g_live_reflex_mutex);
  Reflex* reflex = g_live_reflex;
  if (reflex && tls_input_frame_id) reflex->SimulationEnd(tls_input_frame_id);
  tls_input_frame_id = 0;
  tls_input_sample_marked = false;
  g_input_frame_open.store(false, std::memory_order_release);
}

uint64_t Fm4ReflexCurrentFrameId() {
  return g_current_frame_id.load(std::memory_order_acquire);
}

}  // namespace rex::ui::vulkan
