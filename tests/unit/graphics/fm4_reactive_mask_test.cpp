/**
 * @file        tests/unit/graphics/fm4_reactive_mask_test.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/fm4_reactive_mask.h>

using namespace rex::graphics;

// RB_DEPTHCONTROL, RB_BLENDCONTROL0 and RB_COLOR_MASK of draws logged on a race's scene at 1x.
TEST_CASE("Reactive scene draws are alpha-blended over the scene without depth writes",
          "[reactive]") {
  // Billboards (64x64 texture, quad lists): source alpha over one minus source alpha.
  CHECK(Fm4ReactiveSceneDraw(0x087087E3, 0x07060706, 0xF, true, true));
  // Premultiplied quads: one over one minus source alpha.
  CHECK(Fm4ReactiveSceneDraw(0x00700262, 0x07010701, 0xF, true, true));
  // Only the colour factors decide; the alpha channel's are the game's own business.
  CHECK(Fm4ReactiveSceneDraw(0x087087E3, 0x01060706, 0xF, true, true));
}

TEST_CASE("Reactive scene draws leave out what keeps its own history", "[reactive]") {
  // Opaque track geometry: blending off and depth written.
  CHECK_FALSE(Fm4ReactiveSceneDraw(0x00700267, 0x00010001, 0xF, true, true));
  // Alpha-blended, but writing depth: the object slots give it motion.
  CHECK_FALSE(Fm4ReactiveSceneDraw(0x00700267, 0x07060706, 0xF, true, true));
  // Car glass: a multiplied tint and an additive reflection.
  CHECK_FALSE(Fm4ReactiveSceneDraw(0x087087E3, 0x01060008, 0xF, true, true));
  CHECK_FALSE(Fm4ReactiveSceneDraw(0x087087E3, 0x01060101, 0xF, true, true));
  // Without a depth test the scene's depth says nothing about what covers it.
  CHECK_FALSE(Fm4ReactiveSceneDraw(0x00700261, 0x07060706, 0xF, true, true));
  // Colour writes off.
  CHECK_FALSE(Fm4ReactiveSceneDraw(0x087087E3, 0x07060706, 0x0, true, true));
  // Another surface: a reflection, a shadow or a top-down road map.
  CHECK_FALSE(Fm4ReactiveSceneDraw(0x087087E3, 0x07060706, 0xF, false, true));
  CHECK_FALSE(Fm4ReactiveSceneDraw(0x087087E3, 0x07060706, 0xF, true, false));
}

namespace {
xenos::xe_gpu_texture_fetch_t Fetch(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3,
                                    uint32_t w4, uint32_t w5) {
  xenos::xe_gpu_texture_fetch_t fetch;
  fetch.dword_0 = w0;
  fetch.dword_1 = w1;
  fetch.dword_2 = w2;
  fetch.dword_3 = w3;
  fetch.dword_4 = w4;
  fetch.dword_5 = w5;
  return fetch;
}
}  // namespace

// Fetch constants logged on a race's and a 512 grey map race's scene draws at 1x.
TEST_CASE("The ground shadow map is a square 2D RGBA8 texture with a white border",
          "[reactive]") {
  // Races: 256x256 with mips (1BF83000, the track draws' t11).
  CHECK(Fm4GroundShadowMap(
      Fetch(0x8206D802, 0x1BF83086, 0x001FE0FF, 0x00A80C14, 0x003E80C3, 0x1BFC3A19)));
  // 512x512 (the showroom's size).
  CHECK(Fm4GroundShadowMap(
      Fetch(0x8406D802, 0x163E0086, 0x003FE1FF, 0x00A80C14, 0x003E8103, 0x164E0A19)));
}

TEST_CASE("Other textures are not the ground shadow map", "[reactive]") {
  // A track's colour texture (DXT, 512x256).
  CHECK_FALSE(Fm4GroundShadowMap(
      Fetch(0x840000FE, 0x0F15D054, 0x001FE1FF, 0x06280D10, 0x003DAE43, 0x0F14BA18)));
  // An RGBA8 sign, 384x128.
  CHECK_FALSE(Fm4GroundShadowMap(
      Fetch(0x83024802, 0x11590086, 0x000FE17F, 0x00A80C14, 0x00000003, 0x00000218)));
  // An RGBA8 64x64 texture with a black border.
  CHECK_FALSE(Fm4GroundShadowMap(
      Fetch(0x00824802, 0x115C9086, 0x0007E03F, 0x00280C14, 0x00000183, 0x115CDA18)));
  // An unset fetch constant.
  CHECK_FALSE(Fm4GroundShadowMap(Fetch(0, 0, 0, 0, 0, 0)));
}
