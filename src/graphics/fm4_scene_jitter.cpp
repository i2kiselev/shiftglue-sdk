#include <rex/graphics/fm4_scene_jitter.h>

namespace rex::graphics {

Fm4DrawPlacement Fm4JitterPlacement(const uint64_t (&float_bitmap)[4]) {
  if (((float_bitmap[128 / 64] >> (128 % 64)) & 0xF) == 0xF) return Fm4DrawPlacement::kCamera;
  // c0, c1 and c3: x, y and the translation of the object's matrix.
  if ((float_bitmap[0] & 0xB) == 0xB) return Fm4DrawPlacement::kObject;
  return Fm4DrawPlacement::kUnplaced;
}

bool Fm4SameColorSurface(uint64_t a, uint64_t b) {
  constexpr uint64_t kSurfaceBits = (uint64_t(1) << 40) - 1;
  return ((a ^ b) & kSurfaceBits) == 0;
}

bool Fm4JitterSceneDraw(Fm4DrawPlacement placement, bool tests_scene_depth,
                        bool writes_scene_color) {
  return placement != Fm4DrawPlacement::kUnplaced && (tests_scene_depth || writes_scene_color);
}

}  // namespace rex::graphics
