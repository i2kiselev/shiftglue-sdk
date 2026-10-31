/**
 * @file        graphics/fm4_reactive_mask.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/fm4_reactive_mask.h>

namespace rex::graphics {

bool Fm4ReactiveSceneDraw(uint32_t depth_control, uint32_t blend_control, uint32_t color_mask,
                          bool tests_scene_depth, bool writes_scene_color) {
  constexpr uint32_t kZEnable = 1u << 1, kZWriteEnable = 1u << 2;
  constexpr uint32_t kOne = 1, kSrcAlpha = 6, kOneMinusSrcAlpha = 7, kAdd = 0;
  const uint32_t source = blend_control & 0x1F;
  const uint32_t combine = (blend_control >> 5) & 0x7;
  const uint32_t destination = (blend_control >> 8) & 0x1F;
  return tests_scene_depth && writes_scene_color && (color_mask & 0xF) &&
         (depth_control & (kZEnable | kZWriteEnable)) == kZEnable &&
         (source == kOne || source == kSrcAlpha) && combine == kAdd &&
         destination == kOneMinusSrcAlpha;
}

bool Fm4GroundShadowMap(const xenos::xe_gpu_texture_fetch_t& fetch) {
  return fetch.type == xenos::FetchConstantType::kTexture &&
         fetch.format == xenos::TextureFormat::k_8_8_8_8 &&
         fetch.dimension == xenos::DataDimension::k2DOrStacked && !fetch.stacked &&
         fetch.size_2d.width == fetch.size_2d.height &&
         fetch.border_color == xenos::BorderColor::k_ABGR_White;
}

}  // namespace rex::graphics
