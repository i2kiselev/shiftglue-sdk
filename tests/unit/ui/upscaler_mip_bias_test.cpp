#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/ui/upscaler.h>

using Catch::Approx;
using rex::ui::TemporalUpscalerMipBias;

TEST_CASE("The mip bias picks the output size's mips", "[mip_bias]") {
  CHECK(TemporalUpscalerMipBias(1280, 2560, 0.0f) == Approx(-1.0f));
  CHECK(TemporalUpscalerMipBias(1280, 3840, 0.0f) == Approx(-1.585f).margin(0.001f));
  CHECK(TemporalUpscalerMipBias(2560, 2560, 0.0f) == Approx(0.0f));
}

TEST_CASE("The mip bias offset adds to it, even without an upscale", "[mip_bias]") {
  CHECK(TemporalUpscalerMipBias(1280, 2560, -1.0f) == Approx(-2.0f));
  CHECK(TemporalUpscalerMipBias(2560, 2560, -1.0f) == Approx(-1.0f));
  CHECK(TemporalUpscalerMipBias(1280, 2560, 0.5f) == Approx(-0.5f));
}

TEST_CASE("A render larger than the output keeps the mips the game asks for", "[mip_bias]") {
  CHECK(TemporalUpscalerMipBias(5120, 2560, 0.0f) == Approx(0.0f));
}
