/**
 * @file        tests/unit/graphics/pixel_shader_constant_override_test.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>

#include <rex/graphics/pixel_shader_constant_override.h>

using rex::graphics::ApplyConstantOverride;
using rex::graphics::ParsePixelShaderConstantOverrides;
using rex::graphics::PixelShaderConstantOverride;

TEST_CASE("An empty constant override is none", "[ps_constant_override]") {
  const auto parsed = ParsePixelShaderConstantOverrides("");
  REQUIRE(parsed);
  CHECK(parsed->empty());
}

TEST_CASE("The whole-vector constant override form is unchanged", "[ps_constant_override]") {
  const auto parsed = ParsePixelShaderConstantOverrides("B85EF04779ABD438:c57=1,1,0,0");
  REQUIRE(parsed);
  REQUIRE(parsed->size() == 1);
  const PixelShaderConstantOverride& entry = parsed->front();
  CHECK(entry.shader_hash == 0xB85EF04779ABD438ull);
  CHECK(entry.constant == 57);
  CHECK(entry.component_mask == 0xF);
  CHECK(entry.value == std::array<float, 4>{1.0f, 1.0f, 0.0f, 0.0f});
}

TEST_CASE("A constant override can name one component", "[ps_constant_override]") {
  const auto parsed = ParsePixelShaderConstantOverrides("B85EF04779ABD438:c57.y=0");
  REQUIRE(parsed);
  REQUIRE(parsed->size() == 1);
  CHECK(parsed->front().constant == 57);
  CHECK(parsed->front().component_mask == 0x2);
  CHECK(parsed->front().value[1] == 0.0f);
}

TEST_CASE("A constant override can name two components", "[ps_constant_override]") {
  const auto parsed = ParsePixelShaderConstantOverrides("B85EF04779ABD438:c5.xw=0,1");
  REQUIRE(parsed);
  REQUIRE(parsed->size() == 1);
  CHECK(parsed->front().component_mask == 0x9);
  CHECK(parsed->front().value[0] == 0.0f);
  CHECK(parsed->front().value[3] == 1.0f);
}

TEST_CASE("Constant override entries are kept in order", "[ps_constant_override]") {
  const auto parsed =
      ParsePixelShaderConstantOverrides("79B4914F27ECEC73:c5=0,0,0,1;B85EF04779ABD438:c57.y=0");
  REQUIRE(parsed);
  REQUIRE(parsed->size() == 2);
  CHECK((*parsed)[0].shader_hash == 0x79B4914F27ECEC73ull);
  CHECK((*parsed)[0].component_mask == 0xF);
  CHECK((*parsed)[1].shader_hash == 0xB85EF04779ABD438ull);
  CHECK((*parsed)[1].component_mask == 0x2);
}

TEST_CASE("One bad constant override entry rejects them all", "[ps_constant_override]") {
  CHECK_FALSE(ParsePixelShaderConstantOverrides("B85EF04779ABD438:c57.y=0;garbage"));
}

TEST_CASE("Bad constant override forms are rejected", "[ps_constant_override]") {
  CHECK_FALSE(ParsePixelShaderConstantOverrides("B85EF04779ABD438:c256=0,0,0,0"));
  CHECK_FALSE(ParsePixelShaderConstantOverrides("B85EF04779ABD438:c57.q=0"));
  CHECK_FALSE(ParsePixelShaderConstantOverrides("B85EF04779ABD438:c57.yx=0,0"));
  CHECK_FALSE(ParsePixelShaderConstantOverrides("B85EF04779ABD438:c57.y=0,0"));
  CHECK_FALSE(ParsePixelShaderConstantOverrides("B85EF04779ABD438:c57=0,0,0"));
  CHECK_FALSE(ParsePixelShaderConstantOverrides("B85EF04779ABD438:c57.y=0;"));
}

TEST_CASE("A masked constant override keeps the other components", "[ps_constant_override]") {
  std::array<uint32_t, 4> registers = {
      std::bit_cast<uint32_t>(1.0f), std::bit_cast<uint32_t>(0.5f),
      std::bit_cast<uint32_t>(0.25f), std::bit_cast<uint32_t>(0.125f)};
  const auto parsed = ParsePixelShaderConstantOverrides("B85EF04779ABD438:c57.y=0");
  REQUIRE(parsed);
  REQUIRE(parsed->size() == 1);
  ApplyConstantOverride(parsed->front(), registers);
  CHECK(registers == std::array<uint32_t, 4>{std::bit_cast<uint32_t>(1.0f),
                                             std::bit_cast<uint32_t>(0.0f),
                                             std::bit_cast<uint32_t>(0.25f),
                                             std::bit_cast<uint32_t>(0.125f)});
}
