/**
 * @file        rex/input/mnk/mnk_input_driver.h
 * @brief       Keyboard/mouse input driver - maps MnK to Xbox 360 controller.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <rex/input/input_driver.h>
#include <rex/input/mnk/mnk_bind.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>

namespace rex::input::mnk {

void SetMouseLookActive(bool active);
bool IsMouseLookActive();
// A host dialog the player clicks in (console, settings, achievements) is open:
// mouse look is off and the cursor free until it closes.
void SetHostDialogOpen(bool open);

// Hold look: while the right mouse button is held and mouse look is active,
// horizontal mouse travel turns a full-strength right stick around from
// straight up, and the pointer is locked. Off while mnk_mouse or
// mnk_mouse_steering owns the mouse.
struct HoldLookSettings {
  bool enabled = false;
  double degrees_per_count = 0.15;
  // Hide the cursor whenever mouse look is active, held or not.
  bool hide_cursor = false;
};
void SetHoldLook(const HoldLookSettings& settings);
// Whether the last state poll sent the hold-look stick.
bool IsHoldLookEngaged();

class MnkInputDriver final : public InputDriver,
                             public rex::ui::WindowInputListener,
                             public rex::ui::WindowListener {
 public:
  explicit MnkInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~MnkInputDriver() override;

  X_STATUS Setup() override;
  bool is_keyboard_and_mouse() const override { return true; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  void OnWindowAvailable(rex::ui::Window* window) override;

  // WindowInputListener
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

 private:
  bool IsEnabled() const;
  void SetKeyState(uint16_t vk, bool down);
  void EnqueueKeystroke(uint16_t vk_pad, bool down);

  enum class MouseCapture : uint8_t { kNone, kHidden, kLocked };

  // Called from the guest thread. The rest of the capture path stays on the UI
  // thread, since every Window call in it reaches SDL.
  void QueueMouseCaptureUpdate(MouseCapture capture);
  void ApplyMouseCaptureFromUIThread();
  void ReleaseMouseCaptureFromUIThread(rex::ui::Window* window);
  void RecenterCursorFromUIThread(int32_t x, int32_t y);
  // Safe to call from any thread.
  void DetachFromWindow();

  // Only the UI thread writes it, so only guest thread access needs the lock.
  rex::ui::Window* attached_window_ = nullptr;

  std::mutex state_mutex_;
  bool key_down_[256] = {};
  // Keys held across a change of keybind overrides (FM4's context switch).
  HeldKeyMask held_keys_;
  // Preserve a button transition until at least one guest state poll observes
  // it. Without this, a short host key tap may begin and end between polls.
  uint16_t latched_buttons_ = 0;

  // Mouse delta tracking. Fractional because relative motion arrives in
  // fractions of a pixel, and truncating each event drops slow movement.
  // Filled on the UI thread, drained on the guest thread, hence the lock.
  float mouse_dx_ = 0.0f;
  float mouse_dy_ = 0.0f;
  // Mouse steering position in [-1, 1] and when it was last advanced.
  double steering_ = 0.0;
  std::chrono::steady_clock::time_point last_state_time_ = std::chrono::steady_clock::now();
  // Hold look: engaged only by a press made while it was allowed.
  bool hold_look_engaged_ = false;
  bool right_button_was_down_ = false;
  double hold_look_radians_ = 0.0;

  // UI thread only.
  int32_t prev_mouse_x_ = 0;
  int32_t prev_mouse_y_ = 0;
  MouseCapture mouse_capture_ = MouseCapture::kNone;
  // Whether the window has the pointer locked and is reporting relative motion.
  bool relative_mouse_mode_ = false;
  // Cursor visibility to restore on capture release - the window owner may run
  // an auto-hide policy that capture must not permanently override.
  rex::ui::Window::CursorVisibility precapture_cursor_visibility_ =
      rex::ui::Window::CursorVisibility::kVisible;

  // Guest thread to UI thread. The queued flag coalesces the posts.
  std::atomic<MouseCapture> mouse_capture_requested_{MouseCapture::kNone};
  std::atomic<bool> mouse_capture_update_queued_{false};

  std::atomic<bool> has_focus_{true};

  // Keystroke queue
  std::queue<X_INPUT_KEYSTROKE> keystroke_queue_;

  // Packet number incremented on state change
  uint32_t packet_number_ = 0;
};

}  // namespace rex::input::mnk
