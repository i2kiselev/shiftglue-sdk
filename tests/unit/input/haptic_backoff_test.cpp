#include <catch2/catch_test_macros.hpp>

#include <rex/input/sdl/haptic_backoff.h>

using rex::input::sdl::HapticBackoff;

TEST_CASE("Haptic back-off reopens every 2 s and gives up after 5", "[input][wheel]") {
  HapticBackoff b;
  CHECK_FALSE(b.ShouldReopen(0));
  b.OnFailure(1000);
  CHECK_FALSE(b.ShouldReopen(2999));
  CHECK(b.ShouldReopen(3000));
  for (int i = 0; i < 5; ++i) {
    b.NoteReopened(3000 + i * 2000);
    b.OnFailure(3001 + i * 2000);
  }
  CHECK(b.GaveUp());
  CHECK_FALSE(b.ShouldReopen(100000));
  b.Reset();
  CHECK_FALSE(b.GaveUp());
}

TEST_CASE("A success clears the failed reopens", "[input][wheel]") {
  HapticBackoff b;
  b.OnFailure(0);
  b.NoteReopened(2000);
  b.OnFailure(2001);
  b.OnSuccess();
  CHECK_FALSE(b.ShouldReopen(10000));
  CHECK_FALSE(b.GaveUp());
}

TEST_CASE("Nothing is sent while a failure waits for its reopen", "[input][wheel]") {
  HapticBackoff b;
  CHECK_FALSE(b.Failing());
  b.OnFailure(0);
  CHECK(b.Failing());
  b.NoteReopened(2000);
  CHECK_FALSE(b.Failing());
}
