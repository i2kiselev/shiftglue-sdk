#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/fm4_resolve_readbacks.h>

using namespace rex::graphics;

namespace {

constexpr uint32_t kAddress = 0x1C0C1000;
constexpr uint32_t kLength = 0x20000;

struct Readbacks {
  Fm4ResolveReadbacks readbacks;

  Fm4ResolveReadbacks::Decision A(uint64_t frame, bool read_all = false) {
    return readbacks.Resolve(kAddress, kLength, frame, read_all);
  }
  void Frames(uint64_t first, uint64_t last) {
    for (uint64_t frame = first; frame <= last; ++frame) A(frame);
  }
};

}  // namespace

TEST_CASE("a new range is read back in the first frames of its run", "[graphics][readbacks]") {
  Readbacks r;
  for (uint64_t frame = 1; frame <= 1000; ++frame) {
    CAPTURE(frame);
    CHECK(r.A(frame).read_back == (frame <= 2));
  }
}

TEST_CASE("a run after a short gap is read back while the range is new",
          "[graphics][readbacks]") {
  Readbacks r;
  r.Frames(1, 3);
  const auto gap = r.A(9);
  CHECK(gap.run_start);
  CHECK(gap.read_back);
  const auto late = r.A(200);
  CHECK(late.run_start);
  CHECK_FALSE(late.read_back);
  const auto next = r.A(201);
  CHECK_FALSE(next.run_start);
  CHECK_FALSE(next.read_back);
}

TEST_CASE("a range idle longer than kIdleFrames is new again", "[graphics][readbacks]") {
  Readbacks r;
  r.A(1);
  const auto again = r.A(2 + Fm4ResolveReadbacks::kIdleFrames);
  CHECK(again.run_start);
  CHECK(again.read_back);
}

TEST_CASE("a range another readback wrote over is new again", "[graphics][readbacks]") {
  SECTION("overlapping") {
    Readbacks r;
    r.Frames(1, 10);
    CHECK(r.readbacks.Resolve(kAddress, 0x60000, 150, false).read_back);
    r.readbacks.Resolve(kAddress, 0x60000, 151, false);
    const auto decision = r.A(200);
    CHECK(decision.read_back);
    CHECK(decision.overwritten);
  }
  SECTION("elsewhere") {
    Readbacks r;
    r.Frames(1, 10);
    CHECK(r.readbacks.Resolve(0x1C200000, 0x1000, 150, false).read_back);
    const auto decision = r.A(200);
    CHECK_FALSE(decision.read_back);
    CHECK_FALSE(decision.overwritten);
  }
}

TEST_CASE("an overlapping readback from below counts too", "[graphics][readbacks]") {
  Readbacks r;
  r.Frames(1, 10);
  CHECK(r.readbacks.Resolve(0x1C0B0000, 0x20000, 150, false).read_back);
  const auto decision = r.A(200);
  CHECK(decision.read_back);
  CHECK(decision.overwritten);
}

TEST_CASE("a range resolved more often than in the frame before is read back",
          "[graphics][readbacks]") {
  Readbacks r;
  r.Frames(1, 10);
  CHECK_FALSE(r.A(11).read_back);
  CHECK(r.A(11).read_back);
}

TEST_CASE("read_all reads every resolve back", "[graphics][readbacks]") {
  Readbacks r;
  r.Frames(1, 49);
  CHECK_FALSE(r.A(50).read_back);
  CHECK(r.A(51, true).read_back);
}

TEST_CASE("only a first run is waited for", "[graphics][readbacks]") {
  Readbacks r;
  r.A(1);
  CHECK(r.readbacks.IsFirstRun(kAddress, kLength));
  r.Frames(2, 3);
  r.A(9);
  r.Frames(200, 201);
  CHECK_FALSE(r.readbacks.IsFirstRun(kAddress, kLength));
  r.A(502);
  CHECK(r.readbacks.IsFirstRun(kAddress, kLength));
}

TEST_CASE("a range never resolved counts as a first run", "[graphics][readbacks]") {
  const Fm4ResolveReadbacks readbacks;
  CHECK(readbacks.IsFirstRun(kAddress, kLength));
}

TEST_CASE("readbacks wait for the GPU unless async runs free", "[graphics][readbacks]") {
  CHECK(Fm4ReadbacksWaitForGpu(false, false, false));
  CHECK(Fm4ReadbacksWaitForGpu(true, true, false));
  CHECK(Fm4ReadbacksWaitForGpu(true, false, true));
  CHECK_FALSE(Fm4ReadbacksWaitForGpu(true, false, false));
}
