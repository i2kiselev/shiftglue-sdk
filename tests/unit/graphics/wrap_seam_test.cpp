#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/shader/wrap_seam.h>

using rex::graphics::ParseShaderHashList;
using rex::graphics::SelectWrapSeamBits;
using rex::graphics::WrapSeamBits;
using rex::graphics::WrapSeamShaders;

namespace {

constexpr uint64_t kListedVertex = 0x8D05A46C3584CA9Eull;
constexpr uint64_t kListedPixel = 0xB85EF04779ABD438ull;
constexpr uint64_t kOtherVertex = 0xA42BEA3C00000000ull;
constexpr uint64_t kOtherPixel = 0x0123456789ABCDEFull;

WrapSeamShaders Listed() { return {{kListedVertex}, {kListedPixel}}; }

}  // namespace

TEST_CASE("A wrap seam hash list parses", "[wrap_seam]") {
  const auto parsed = ParseShaderHashList("8D05A46C3584CA9E, 5C8D739C61E85EC7");
  REQUIRE(parsed);
  CHECK(parsed->size() == 2);
  CHECK(parsed->count(0x8D05A46C3584CA9Eull));
  CHECK(parsed->count(0x5C8D739C61E85EC7ull));
}

TEST_CASE("An empty wrap seam hash list is no shaders", "[wrap_seam]") {
  const auto parsed = ParseShaderHashList("");
  REQUIRE(parsed);
  CHECK(parsed->empty());
}

TEST_CASE("A bad wrap seam hash rejects the list", "[wrap_seam]") {
  CHECK_FALSE(ParseShaderHashList("8D05A46C3584CA9E,xyz"));
  CHECK_FALSE(ParseShaderHashList("8D05A46C3584CA9E,"));
}

TEST_CASE("The wrap seam fix off sets no bits", "[wrap_seam]") {
  const WrapSeamBits bits = SelectWrapSeamBits(Listed(), false, kListedVertex, kListedPixel, false);
  CHECK_FALSE(bits.vertex);
  CHECK_FALSE(bits.pixel);
}

TEST_CASE("A listed wrap seam pair gets both bits", "[wrap_seam]") {
  const WrapSeamBits bits = SelectWrapSeamBits(Listed(), true, kListedVertex, kListedPixel, false);
  CHECK(bits.vertex);
  CHECK(bits.pixel);
}

TEST_CASE("A listed vertex shader with an unlisted pixel shader gets the vertex bit",
          "[wrap_seam]") {
  const WrapSeamBits bits = SelectWrapSeamBits(Listed(), true, kListedVertex, kOtherPixel, false);
  CHECK(bits.vertex);
  CHECK_FALSE(bits.pixel);
}

TEST_CASE("The wrap seam pixel bit needs a listed vertex shader", "[wrap_seam]") {
  const WrapSeamBits bits = SelectWrapSeamBits(Listed(), true, kOtherVertex, kListedPixel, false);
  CHECK_FALSE(bits.vertex);
  CHECK_FALSE(bits.pixel);
}

TEST_CASE("A listed vertex shader without a pixel shader gets the vertex bit", "[wrap_seam]") {
  const WrapSeamBits bits = SelectWrapSeamBits(Listed(), true, kListedVertex, 0, false);
  CHECK(bits.vertex);
  CHECK_FALSE(bits.pixel);
}

TEST_CASE("The wrap seam bits follow the switch", "[wrap_seam]") {
  const WrapSeamBits on = SelectWrapSeamBits(Listed(), true, kListedVertex, kListedPixel, false);
  const WrapSeamBits off = SelectWrapSeamBits(Listed(), false, kListedVertex, kListedPixel, false);
  CHECK((on.vertex && on.pixel));
  CHECK_FALSE((off.vertex || off.pixel));
}

TEST_CASE("A draw with point parameters gets no wrap seam bits", "[wrap_seam]") {
  const WrapSeamBits bits = SelectWrapSeamBits(Listed(), true, kListedVertex, kListedPixel, true);
  CHECK_FALSE(bits.vertex);
  CHECK_FALSE(bits.pixel);
}
