#pragma once
// Which of FM4's draws mark a temporal upscaler's reactive mask (a debug option). Blended
// draws test the scene's depth without writing it, so they take the motion of what is behind
// them; marked pixels lean on the current frame instead. In races and replays they are car
// glass, tail lights and tyre marks: the replay blocks DLSS's newer models showed behind cars
// were not particles. The car's ground shadow can be marked too (it moves with the car over
// ground that moves with the camera), and the blocks sit on it, but DLSS (presets K and M)
// ignores the mask altogether: a mask of 1 everywhere leaves its output unchanged, while FSR 3
// turns into the raw frame. FSR 3 shows no blocks there.

#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics {

// Whether a draw marks the mask, from its RB_DEPTHCONTROL, RB_BLENDCONTROL0 and RB_COLOR_MASK:
// testing the scene's depth without writing it, onto the scene's colour surface, alpha-blended
// (source alpha or one, over one minus source alpha). Additive and multiplied blends (car
// glass) move with what they cover and stay out.
bool Fm4ReactiveSceneDraw(uint32_t depth_control, uint32_t blend_control, uint32_t color_mask,
                          bool tests_scene_depth, bool writes_scene_color);

// The fetch constant FM4's track draws sample the car's ground shadow map from: the "grey" map,
// the car seen from the sun (256x256 in races, 512x512 in the showroom), 1 where the ground is
// lit.
constexpr uint32_t kFm4GroundShadowFetchConstant = 11;

// Whether a fetch constant holds the ground shadow map: a square 2D k_8_8_8_8 texture with a
// white border (outside the map the ground is lit).
bool Fm4GroundShadowMap(const xenos::xe_gpu_texture_fetch_t& fetch);

}  // namespace rex::graphics
