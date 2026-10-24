#pragma once
// Which resolves the FH1 native executor reads back into guest memory for FM4,
// over plain data so the rule is testable without a GPU.

#include <cstdint>
#include <map>

namespace rex::graphics {

class Fm4ResolveReadbacks {
 public:
  static constexpr uint64_t kRunGapFrames = 4, kRunReadbackFrames = 2, kIdleFrames = 300,
                            kNewFrames = 120;
  struct Decision {
    bool read_back = false;
    bool run_start = false;
    bool overwritten = false;
    bool read_back_before = false;  // the range had a readback before this resolve
    bool log = false;               // a run start, or the frame its readbacks end
    uint64_t idle_frames = 0;       // at a run start: frames since the range's last resolve
    uint64_t new_since_frames = 0;
    uint64_t previous_run_frames = 0;
  };
  // One resolve to [address, address + length) in `frame` (frames count from 1).
  Decision Resolve(uint32_t address, uint32_t length, uint64_t frame, bool read_all);
  // Whether the range's current run began while it was new, so the guest may process it at
  // once and an async readback still waits for it; true for a range never resolved.
  bool IsFirstRun(uint32_t address, uint32_t length) const;

 private:
  struct Range {
    uint64_t last_used_frame = 0;
    uint64_t run_start_frame = 0;
    uint64_t new_since_frame = 0;
    // Order of the range's latest readback among all readbacks, 0 if none.
    uint64_t readback_sequence = 0;
    // Resolves this frame (count_frame) and in the frame before it.
    uint64_t count_frame = UINT64_MAX;
    uint32_t count = 0;
    uint32_t previous_count = 0;
  };
  // Ordered by address << 32 | length: the overwrite scan walks from the start.
  std::map<uint64_t, Range> ranges_;
  uint64_t readback_sequence_ = 0;
};

// Whether pending readbacks are waited for: always without async readbacks, while they are
// held (menus), or while one of them is a first run.
bool Fm4ReadbacksWaitForGpu(bool async, bool held, bool first_run_pending);

}  // namespace rex::graphics
