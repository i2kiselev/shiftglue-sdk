/**
 * @file        tests/unit/graphics/fm4_decal_motion_test.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/fm4_decal_motion.h>

using namespace rex::graphics;

// RB_DEPTHCONTROL, RB_BLENDCONTROL0 and RB_COLOR_MASK of draws logged on a replay's scene at 2x
// (Maple Valley, the cinematic start).
TEST_CASE("A car part's ground shadow multiplies the scene without writing depth",
          "[decal_motion]") {
  // The body's shadow over the ground (vs CC292E3F, no depth test) and a wheel's (F2E1966A).
  CHECK(Fm4ShadowDecalDraw(0x00714265, 0x01060008, 0xF, true));
  // The same product written the other way round.
  CHECK(Fm4ShadowDecalDraw(0x00714265, 0x01060400, 0xF, true));
  // Car glass's tint: depth tested, multiplied, on the car; it moves with the car as well.
  CHECK(Fm4ShadowDecalDraw(0x087087E3, 0x01060008, 0xF, true));
}

TEST_CASE("Other car draws are not ground shadows", "[decal_motion]") {
  // The body: opaque, writing depth (it has an object slot of its own).
  CHECK_FALSE(Fm4ShadowDecalDraw(0x087087E7, 0x01060706, 0xF, true));
  // Multiplied but writing depth.
  CHECK_FALSE(Fm4ShadowDecalDraw(0x087087E7, 0x01060008, 0xF, true));
  // Glass's additive reflection, and an alpha-blended quad.
  CHECK_FALSE(Fm4ShadowDecalDraw(0x087087E3, 0x01060101, 0xF, true));
  CHECK_FALSE(Fm4ShadowDecalDraw(0x087087E3, 0x07060706, 0xF, true));
  // The stencil boxes around each car's shadow (vs 16F25D85): blending off.
  CHECK_FALSE(Fm4ShadowDecalDraw(0x08714263, 0x00010001, 0xF, true));
  // Colour writes off, or another surface.
  CHECK_FALSE(Fm4ShadowDecalDraw(0x00714265, 0x01060008, 0x0, true));
  CHECK_FALSE(Fm4ShadowDecalDraw(0x00714265, 0x01060008, 0xF, false));
}

TEST_CASE("Car shaders read their part's world matrix at c8-c11", "[decal_motion]") {
  const auto bitmap = [](std::initializer_list<std::pair<uint32_t, uint32_t>> ranges) {
    uint64_t words[4] = {};
    for (const auto& [first, last] : ranges) {
      for (uint32_t c = first; c <= last; ++c) words[c >> 6] |= uint64_t(1) << (c & 63);
    }
    return std::to_array(words);
  };
  // The body (vs 8D05A46C), a tyre (C0054227) and the body's shadow (CC292E3F).
  CHECK(Fm4ReadsWorldMatrix(bitmap({{0, 14}, {32, 33}, {249, 255}})));
  CHECK(Fm4ReadsWorldMatrix(bitmap({{0, 3}, {8, 14}, {32, 33}, {255, 255}})));
  CHECK(Fm4ReadsWorldMatrix(bitmap({{0, 3}, {8, 11}, {32, 33}, {35, 35}})));
  // The driver (an indexed bone palette, c0-c3 only) and the stencil boxes.
  CHECK_FALSE(Fm4ReadsWorldMatrix(bitmap({{0, 3}, {252, 255}})));
  CHECK_FALSE(Fm4ReadsWorldMatrix(bitmap({{0, 3}})));
  // Part of the block is not the matrix.
  CHECK_FALSE(Fm4ReadsWorldMatrix(bitmap({{0, 3}, {8, 10}})));
}
