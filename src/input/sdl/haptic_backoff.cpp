/**
 * @file        input/sdl/haptic_backoff.cpp
 * @brief       When to reopen a wheel's force feedback after the driver refuses a call.
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/sdl/haptic_backoff.h>

namespace rex::input::sdl {

void HapticBackoff::OnFailure(uint64_t now_ms) {
  failing_ = true;
  last_ms_ = now_ms;
  if (reopens_ >= kMaxFailedReopens) {
    gave_up_ = true;
  }
}

void HapticBackoff::OnSuccess() {
  failing_ = false;
  reopens_ = 0;
}

bool HapticBackoff::ShouldReopen(uint64_t now_ms) const {
  return failing_ && !gave_up_ && now_ms - last_ms_ >= kReopenIntervalMs;
}

void HapticBackoff::NoteReopened(uint64_t now_ms) {
  failing_ = false;
  last_ms_ = now_ms;
  ++reopens_;
}

void HapticBackoff::Reset() {
  *this = HapticBackoff();
}

}  // namespace rex::input::sdl
