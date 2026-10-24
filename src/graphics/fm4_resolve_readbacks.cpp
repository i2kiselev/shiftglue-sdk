#include <rex/graphics/fm4_resolve_readbacks.h>

namespace rex::graphics {

Fm4ResolveReadbacks::Decision Fm4ResolveReadbacks::Resolve(uint32_t address, uint32_t length,
                                                           uint64_t frame, bool read_all) {
  // As the D3D12 executor: the first frames of each run of resolves to a new
  // range are read back: car thumbnails the game saves, the 32x512 blurred
  // asphalt strips FM4 compresses on the CPU at race load, and the 8x8 cube
  // faces no draw samples, which the CPU reads for the cars' ambient light. Any
  // size: with a 64 KB floor those faces kept whatever the memory held before,
  // tinting cars purple once a few races had left garbage there.
  // A range another readback has written over since its own is new again: the
  // showroom renders the 256x128 environment map the CPU reduces to the cars'
  // ambient light into memory it shares with a 768x384 one, and when a car
  // purchase reran the small map within the idle frames only the large one
  // was read back, so the CPU lit every car from its bytes (magenta).
  // So is a resolve to a range already resolved more times this frame than in
  // the previous one: a capture drawn after the frame into one of its own
  // targets, such as the photo mode picture the game encodes (#399).
  const uint64_t key = (uint64_t(address) << 32) | length;
  Range& range = ranges_[key];
  if (range.count_frame != frame) {
    range.previous_count = range.count_frame + 1 == frame ? range.count : 0;
    range.count = 0;
    range.count_frame = frame;
  }
  ++range.count;
  Decision decision;
  decision.run_start = !range.last_used_frame || frame > range.last_used_frame + kRunGapFrames;
  decision.previous_run_frames =
      range.last_used_frame ? range.last_used_frame - range.run_start_frame + 1 : 0;
  if (decision.run_start) {
    if (range.readback_sequence) {
      const uint64_t end = uint64_t(address) + length;
      for (auto other = ranges_.begin(); other != ranges_.end() && (other->first >> 32) < end;
           ++other) {
        if (other->first != key && other->second.readback_sequence > range.readback_sequence &&
            (other->first >> 32) + uint32_t(other->first) > address) {
          decision.overwritten = true;
          break;
        }
      }
    }
    if (!range.last_used_frame || frame > range.last_used_frame + kIdleFrames ||
        decision.overwritten) {
      range.new_since_frame = frame;
    }
    range.run_start_frame = frame;
  }
  if (decision.run_start && range.last_used_frame) {
    decision.idle_frames = frame - range.last_used_frame;
  }
  range.last_used_frame = frame;
  const bool extra = range.previous_count && range.count > range.previous_count;
  decision.read_back = read_all || extra ||
                       (frame < range.run_start_frame + kRunReadbackFrames &&
                        frame < range.new_since_frame + kNewFrames);
  decision.read_back_before = range.readback_sequence != 0;
  decision.log = decision.run_start || frame == range.run_start_frame + kRunReadbackFrames;
  decision.new_since_frames = frame - range.new_since_frame;
  if (decision.read_back) range.readback_sequence = ++readback_sequence_;
  return decision;
}

bool Fm4ResolveReadbacks::IsFirstRun(uint32_t address, uint32_t length) const {
  const auto range = ranges_.find((uint64_t(address) << 32) | length);
  return range == ranges_.end() || range->second.run_start_frame == range->second.new_since_frame;
}

bool Fm4ReadbacksWaitForGpu(bool async, bool held, bool first_run_pending) {
  return !async || held || first_run_pending;
}

}  // namespace rex::graphics
