#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/fm4_scene_jitter.h>

using namespace rex::graphics;

// Float constant bitmaps of vertex shaders logged on the scene's surfaces at 1x.
TEST_CASE("Scene jitter placement by the constants a vertex shader reads", "[jitter]") {
  // World geometry: the camera's matrix in c128-c131.
  const uint64_t world[4] = {0x7, 0, 0x0000000000E0700F, 0x0000330010003000};
  CHECK(Fm4JitterPlacement(world) == Fm4DrawPlacement::kCamera);
  // A car's layer over its first pass: its own matrix in c0-c3.
  const uint64_t car_layer[4] = {0x0000080B00000F0F, 0x3010000000000000, 0, 0x8000000000000000};
  CHECK(Fm4JitterPlacement(car_layer) == Fm4DrawPlacement::kObject);
  const uint64_t matrix_only[4] = {0xF, 0, 0, 0};
  CHECK(Fm4JitterPlacement(matrix_only) == Fm4DrawPlacement::kObject);
  // The showroom floor: a flat mesh, x * c0 + y * c1 + c3.
  const uint64_t flat_mesh[4] = {0x3FB, 0, 0, 0};
  CHECK(Fm4JitterPlacement(flat_mesh) == Fm4DrawPlacement::kObject);
  // Relative addressing marks every constant.
  const uint64_t relative[4] = {~0ull, ~0ull, ~0ull, ~0ull};
  CHECK(Fm4JitterPlacement(relative) == Fm4DrawPlacement::kCamera);
  // Screen-space passes: positions passed through, c0 for something else, or no constants.
  const uint64_t passthrough[4] = {0x1, 0, 0, 0x8000000000000000};
  CHECK(Fm4JitterPlacement(passthrough) == Fm4DrawPlacement::kUnplaced);
  const uint64_t none[4] = {0, 0, 0, 0};
  CHECK(Fm4JitterPlacement(none) == Fm4DrawPlacement::kUnplaced);
  const uint64_t no_translation[4] = {0x7, 0, 0, 0};
  CHECK(Fm4JitterPlacement(no_translation) == Fm4DrawPlacement::kUnplaced);
}

TEST_CASE("Scene jitter colour surfaces ignore the format they are viewed through",
          "[jitter]") {
  // 2_10_10_10_FLOAT_AS_16_16_16_16, 2_10_10_10_FLOAT and 2_10_10_10_AS_10_10_10_10 views of
  // the 4x MSAA scene at base 0, pitch 1280.
  CHECK(Fm4SameColorSurface(0x0C0205000000, 0x030205000000));
  CHECK(Fm4SameColorSurface(0x0A0205000000, 0x0C0205000000));
  // Another sample count, pitch or base is another surface.
  CHECK_FALSE(Fm4SameColorSurface(0x0C0105000000, 0x0C0205000000));
  CHECK_FALSE(Fm4SameColorSurface(0x0C0205000000, 0x0C0202800000));
  CHECK_FALSE(Fm4SameColorSurface(0x0C0205000100, 0x0C0205000000));
}

TEST_CASE("Scene jitter takes placed draws on the scene's depth or colour", "[jitter]") {
  // A car layer testing the scene's depth without writing it.
  CHECK(Fm4JitterSceneDraw(Fm4DrawPlacement::kObject, true, true));
  CHECK(Fm4JitterSceneDraw(Fm4DrawPlacement::kObject, true, false));
  // The sky, drawn without a depth test.
  CHECK(Fm4JitterSceneDraw(Fm4DrawPlacement::kCamera, false, true));
  // A reflection or shadow map.
  CHECK_FALSE(Fm4JitterSceneDraw(Fm4DrawPlacement::kCamera, false, false));
  // A full-screen pass on the scene would leave a strip of the edge unwritten when moved.
  CHECK_FALSE(Fm4JitterSceneDraw(Fm4DrawPlacement::kUnplaced, true, true));
}
