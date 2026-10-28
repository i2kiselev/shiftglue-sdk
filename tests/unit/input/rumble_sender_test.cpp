#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <tuple>
#include <vector>

#include <rex/input/sdl/rumble_sender.h>

using rex::input::sdl::RumbleSender;

namespace {

// A send that blocks until released, like SDL's rumble over Bluetooth.
struct BlockingPad {
  std::mutex mutex;
  std::condition_variable cv;
  bool open = false;
  int started = 0;
  std::vector<std::tuple<uint32_t, uint16_t, uint16_t>> sent;

  RumbleSender::Send Sender() {
    return [this](uint32_t pad, uint16_t left, uint16_t right) {
      std::unique_lock lock(mutex);
      ++started;
      cv.notify_all();
      cv.wait(lock, [this] { return open; });
      sent.emplace_back(pad, left, right);
      cv.notify_all();
    };
  }
  bool WaitStarted(int count) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(5), [&] { return started >= count; });
  }
  bool WaitSent(size_t count) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(5), [&] { return sent.size() >= count; });
  }
  void Open() {
    std::lock_guard lock(mutex);
    open = true;
    cv.notify_all();
  }
};

bool ReturnsSoon(std::function<void()> call) {
  auto done = std::async(std::launch::async, std::move(call));
  return done.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
}

}  // namespace

TEST_CASE("Rumble posts return while a send is still on the wire", "[input][rumble]") {
  BlockingPad pad;
  RumbleSender sender(pad.Sender());
  sender.Post(1, 100, 100);
  REQUIRE(pad.WaitStarted(1));
  CHECK(ReturnsSoon([&] { sender.Post(1, 200, 200); }));
  pad.Open();
  REQUIRE(pad.WaitSent(2));
  sender.Stop();
}

TEST_CASE("Rumble sends only a pad's newest values", "[input][rumble]") {
  BlockingPad pad;
  RumbleSender sender(pad.Sender());
  sender.Post(1, 100, 100);
  REQUIRE(pad.WaitStarted(1));
  sender.Post(1, 200, 200);
  sender.Post(1, 300, 301);
  pad.Open();
  REQUIRE(pad.WaitSent(2));
  sender.Stop();
  REQUIRE(pad.sent.size() == 2);
  CHECK(pad.sent[1] == std::make_tuple(uint32_t(1), uint16_t(300), uint16_t(301)));
}

TEST_CASE("Rumble keeps each pad's values", "[input][rumble]") {
  BlockingPad pad;
  RumbleSender sender(pad.Sender());
  sender.Post(1, 100, 100);
  REQUIRE(pad.WaitStarted(1));
  sender.Post(2, 7, 8);
  sender.Post(1, 9, 10);
  pad.Open();
  REQUIRE(pad.WaitSent(3));
  sender.Stop();
  REQUIRE(pad.sent.size() == 3);
  // Pad 1 posted again while it was on the wire; pad 2 goes first.
  CHECK(std::get<0>(pad.sent[1]) == 2);
  CHECK(std::find(pad.sent.begin(), pad.sent.end(), std::make_tuple(uint32_t(2), uint16_t(7),
                                                                    uint16_t(8))) != pad.sent.end());
  CHECK(std::find(pad.sent.begin(), pad.sent.end(), std::make_tuple(uint32_t(1), uint16_t(9),
                                                                    uint16_t(10))) != pad.sent.end());
}

TEST_CASE("Rumble sends nothing once stopped", "[input][rumble]") {
  BlockingPad pad;
  RumbleSender sender(pad.Sender());
  sender.Post(1, 100, 100);
  REQUIRE(pad.WaitStarted(1));
  sender.Post(1, 200, 200);
  // Stop waits for the send on the wire, so a release sent after it is the last word.
  auto stopped = std::async(std::launch::async, [&] { sender.Stop(); });
  CHECK(stopped.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
  pad.Open();
  REQUIRE(stopped.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  sender.Post(1, 300, 300);
  sender.Stop();
  CHECK(pad.sent.size() == 1);
}
