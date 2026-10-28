#include <rex/input/sdl/rumble_sender.h>

namespace rex::input::sdl {

RumbleSender::RumbleSender(Send send) : send_(std::move(send)), thread_([this] { Run(); }) {}

RumbleSender::~RumbleSender() { Stop(); }

void RumbleSender::Post(uint32_t pad, uint16_t left, uint16_t right) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) {
      return;
    }
    pending_[pad] = {left, right};
  }
  cv_.notify_one();
}

void RumbleSender::Stop() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    pending_.clear();
  }
  cv_.notify_one();
  std::lock_guard join_lock(join_mutex_);
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
    thread_.join();
  }
}

void RumbleSender::Run() {
  std::unique_lock lock(mutex_);
  uint32_t last_pad = 0;
  while (true) {
    cv_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
    if (stopping_) {
      return;
    }
    // Round robin, so a pad that posts every frame cannot starve another.
    auto next = pending_.upper_bound(last_pad);
    if (next == pending_.end()) {
      next = pending_.begin();
    }
    const uint32_t pad = next->first;
    last_pad = pad;
    const auto [left, right] = next->second;
    pending_.erase(next);
    lock.unlock();
    send_(pad, left, right);
    lock.lock();
  }
}

}  // namespace rex::input::sdl
