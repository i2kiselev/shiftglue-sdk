#include <rex/graphics/fm4_decal_motion.h>

namespace rex::graphics {

bool Fm4ShadowDecalDraw(uint32_t depth_control, uint32_t blend_control, uint32_t color_mask,
                        bool writes_scene_color) {
  // The write bit counts only with the test on (the body's shadow sets it with the test off).
  constexpr uint32_t kZEnable = 1u << 1, kZWriteEnable = 1u << 2;
  constexpr uint32_t kZero = 0, kSrcColor = 4, kDestColor = 8, kAdd = 0;
  const uint32_t source = blend_control & 0x1F;
  const uint32_t combine = (blend_control >> 5) & 0x7;
  const uint32_t destination = (blend_control >> 8) & 0x1F;
  return writes_scene_color && (color_mask & 0xF) &&
         (depth_control & (kZEnable | kZWriteEnable)) != (kZEnable | kZWriteEnable) &&
         combine == kAdd &&
         ((source == kDestColor && destination == kZero) ||
          (source == kZero && destination == kSrcColor));
}

bool Fm4ReadsWorldMatrix(const std::array<uint64_t, 4>& float_bitmap) {
  return ((float_bitmap[0] >> 8) & 0xF) == 0xF;
}

}  // namespace rex::graphics
