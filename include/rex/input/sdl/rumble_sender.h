/**
 * @file        rex/input/sdl/rumble_sender.h
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace rex::input::sdl {

// Sends pad rumble on its own thread, only each pad's newest values. Over
// Bluetooth SDL's rumble call waits for the previous report to go out (8-14 ms
// with an 8BitDo Pro 2), and titles set vibration from their main loop: FM4
// hitched on every kerb.
class RumbleSender {
 public:
  using Send = std::function<void(uint32_t pad, uint16_t left, uint16_t right)>;

  explicit RumbleSender(Send send);
  ~RumbleSender();

  void Post(uint32_t pad, uint16_t left, uint16_t right);
  // Drops what is pending and waits for a send in progress: nothing is sent
  // after it returns, so a release sent next is the pad's last word. Final.
  void Stop();

 private:
  void Run();

  Send send_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::map<uint32_t, std::pair<uint16_t, uint16_t>> pending_;
  bool stopping_ = false;
  // Shutdown and the crash path may both stop it.
  std::mutex join_mutex_;
  std::thread thread_;
};

}  // namespace rex::input::sdl
