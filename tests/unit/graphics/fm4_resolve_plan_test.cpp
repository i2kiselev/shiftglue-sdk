#include <cstring>
#include <set>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/fh1_edram_resolve.h>
#include <rex/graphics/register_file.h>

#include "test_memory.h"

using namespace rex::graphics;

namespace {

struct ResolveCase {
  xenos::ColorFormat format = xenos::ColorFormat::k_8_8_8_8;
  xenos::SurfaceNumberFormat number = xenos::SurfaceNumberFormat(0);
  uint32_t array = 0;
  xenos::Endian128 endian = xenos::Endian128(0);
  bool depth = false;
};

// A 32x32 rectangle resolve from color target 0 (k_8_8_8_8) or depth, as
// fh1_edram_resolve_test.cpp sets it up.
Fh1ResolvePlan PlanResolve(const ResolveCase& c) {
  auto& memory = rex::testing::GetTestMemory();
  const uint32_t vertices = memory.SystemHeapAlloc(4096, 4096, rex::memory::kSystemHeapPhysical);
  REQUIRE(vertices);
  const float rectangle[] = {0, 0, 32, 0, 0, 32};
  std::memcpy(memory.TranslatePhysical(vertices), rectangle, sizeof(rectangle));

  RegisterFile regs;
  xenos::xe_gpu_vertex_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kVertex;
  fetch.address = (vertices & 0x1FFFFFFF) >> 2;
  fetch.size = 6;
  std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch, sizeof(fetch));
  regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 32 | (32 << 16);
  regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = 32 | (32 << 16);
  regs[XE_GPU_REG_RB_COPY_DEST_BASE] = 0x100000;
  regs[XE_GPU_REG_RB_COPY_DEST_PITCH] = 32 | (32 << 16);
  regs[XE_GPU_REG_RB_SURFACE_INFO] = 32;
  reg::RB_COLOR_INFO color{};
  color.color_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  regs[XE_GPU_REG_RB_COLOR_INFO] = color.value;
  reg::RB_DEPTH_INFO depth{};
  depth.depth_format = xenos::DepthRenderTargetFormat::kD24S8;
  regs[XE_GPU_REG_RB_DEPTH_INFO] = depth.value;
  reg::RB_COPY_DEST_INFO dest{};
  dest.copy_dest_format = c.format;
  dest.copy_dest_number = c.number;
  dest.copy_dest_array = c.array;
  dest.copy_dest_endian = c.endian;
  regs[XE_GPU_REG_RB_COPY_DEST_INFO] = dest.value;
  reg::RB_COPY_CONTROL control{};
  control.copy_src_select = c.depth ? 4 : 0;
  control.copy_command = xenos::CopyCommand::kConvert;
  control.copy_sample_select = xenos::CopySampleSelect::k0;
  regs[XE_GPU_REG_RB_COPY_CONTROL] = control.value;

  Fh1ResolvePlan plan;
  CHECK(Fh1PlanResolve(regs, memory, {}, plan));
  memory.SystemHeapFree(vertices);
  return plan;
}

}  // namespace

TEST_CASE("each resolve destination format has its own pack and texel size",
          "[graphics][resolve]") {
  struct Expected {
    xenos::ColorFormat format;
    uint32_t pack, bpb_log2;
  };
  const Expected table[] = {
      {xenos::ColorFormat::k_8, 8, 0},
      {xenos::ColorFormat::k_8_8_8_8, 0, 2},
      {xenos::ColorFormat::k_2_10_10_10, 1, 2},
      {xenos::ColorFormat::k_32_FLOAT, 2, 2},
      {xenos::ColorFormat::k_16_16_16_16_FLOAT, 3, 3},
      {xenos::ColorFormat::k_16_16_16_16, 5, 3},
      {xenos::ColorFormat::k_16_16, 6, 2},
      {xenos::ColorFormat::k_8_8, 7, 1},
  };
  std::set<uint32_t> packs;
  for (const auto& e : table) {
    CAPTURE(uint32_t(e.format));
    const auto plan = PlanResolve({.format = e.format});
    CHECK(plan.copy);
    CHECK(plan.skip == nullptr);
    CHECK(Fh1ResolvePack(plan.dest_info) == e.pack);
    CHECK(((plan.dest_info >> 16) & 3) == e.bpb_log2);
    packs.insert(Fh1ResolvePack(plan.dest_info));
  }
  const auto depth = PlanResolve({.depth = true});
  CHECK(depth.copy);
  CHECK(depth.skip == nullptr);
  CHECK(Fh1ResolvePack(depth.dest_info) == 4);
  CHECK(((depth.dest_info >> 16) & 3) == 2);
  packs.insert(Fh1ResolvePack(depth.dest_info));
  CHECK(packs.size() == 9);
}

TEST_CASE("the surface number format never reaches the pack", "[graphics][resolve]") {
  for (const auto [format, pack] :
       {std::pair{xenos::ColorFormat::k_8, 8u}, std::pair{xenos::ColorFormat::k_16_16, 6u}}) {
    for (uint32_t number = 0; number < 8; ++number) {
      CAPTURE(uint32_t(format), number);
      const auto plan =
          PlanResolve({.format = format, .number = xenos::SurfaceNumberFormat(number)});
      CHECK(plan.copy);
      CHECK(Fh1ResolvePack(plan.dest_info) == pack);
      CHECK(((plan.dest_info >> 23) & 7) == number);
    }
  }
}

TEST_CASE("copies the resolve cannot run natively name why", "[graphics][resolve]") {
  const auto array = PlanResolve({.array = 1});
  CHECK_FALSE(array.copy);
  CHECK(std::string_view(array.skip ? array.skip : "") == "resolve_dest_array");
  const auto format = PlanResolve({.format = xenos::ColorFormat::k_5_6_5});
  CHECK_FALSE(format.copy);
  CHECK(std::string_view(format.skip ? format.skip : "") == "resolve_dest_format");
  const auto endian = PlanResolve({.endian = xenos::Endian128(4)});
  CHECK_FALSE(endian.copy);
  CHECK(std::string_view(endian.skip ? endian.skip : "") == "resolve_dest_endian");
}
