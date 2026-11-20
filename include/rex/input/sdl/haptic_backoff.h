#pragma once
/**
 * @file        rex/input/sdl/haptic_backoff.h
 * @brief       When to reopen a wheel's force feedback after the driver refuses a call.
 *
 * Some wheel drivers (Thrustmaster's) refuse every effect call after a while until the device
 * is opened again. After a failure nothing is sent until the haptic device is reopened, every
 * 2 s; after 5 reopens that fail again the wheel is left alone until it is plugged in again.
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>

namespace rex::input::sdl {

class HapticBackoff {
 public:
  static constexpr uint64_t kReopenIntervalMs = 2000;
  static constexpr int kMaxFailedReopens = 5;

  void OnFailure(uint64_t now_ms);
  void OnSuccess();
  bool ShouldReopen(uint64_t now_ms) const;
  void NoteReopened(uint64_t now_ms);
  // A failure waits for its reopen, or the wheel gave up: send nothing.
  bool Failing() const { return failing_ || gave_up_; }
  bool GaveUp() const { return gave_up_; }
  void Reset();

 private:
  bool failing_ = false;
  bool gave_up_ = false;
  int reopens_ = 0;  // since the last success
  uint64_t last_ms_ = 0;
};

}  // namespace rex::input::sdl
