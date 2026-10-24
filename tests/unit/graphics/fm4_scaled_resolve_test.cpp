#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/pipeline/texture/cache.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/shared_memory.h>

#include "test_memory.h"

namespace {

using namespace rex::graphics;

// A copy of texture_cache_test.cpp's harness: that file is upstream's.
class TestSharedMemory final : public SharedMemory {
 public:
  TestSharedMemory() : SharedMemory(rex::testing::GetTestMemory()) { InitializeCommon(); }

 private:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& ranges) override {
    for (const auto& [first, count] : ranges) {
      MakeRangeValid(first << page_size_log2(), count << page_size_log2(), false);
    }
    return true;
  }
};

class TestTextureCache final : public TextureCache {
 public:
  TestTextureCache(const RegisterFile& regs, SharedMemory& memory)
      : TextureCache(regs, memory, 2, 2) {}

  const Texture* bound(uint32_t index = 0) const {
    const auto* binding = GetValidTextureBinding(index);
    return binding ? binding->texture : nullptr;
  }

  bool EnsureScaledResolveMemoryCommitted(uint32_t, uint32_t, uint32_t) override {
    return true;
  }

 private:
  class TestTexture final : public Texture {
   public:
    TestTexture(TextureCache& cache, TextureKey key) : Texture(cache, key, false) {}
  };

  uint32_t GetHostFormatSwizzle(TextureKey) const override {
    return xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension) const override { return 16384; }
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension) const override { return 2048; }
  bool IsScaledResolveSupportedForFormat(TextureKey) const override { return true; }
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override {
    return std::make_unique<TestTexture>(*this, key);
  }
  bool LoadTextureDataFromResidentMemoryImpl(Texture&, bool, bool) override { return true; }
};

constexpr uint32_t kBase = 0x300000;
constexpr uint32_t kMips = 0x310000;

// A 64x64 tiled k_8_8_8_8 with packed mips.
void SetMippedFetch(RegisterFile& regs) {
  xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kTexture;
  fetch.dimension = xenos::DataDimension::k2DOrStacked;
  fetch.format = xenos::TextureFormat::k_8_8_8_8;
  fetch.tiled = 1;
  fetch.packed_mips = 1;
  fetch.pitch = 2;
  fetch.size_2d.width = 63;
  fetch.size_2d.height = 63;
  fetch.base_address = kBase >> 12;
  fetch.mip_address = kMips >> 12;
  fetch.mip_max_level = 6;
  std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch, sizeof(fetch));
}

using Runs = std::vector<std::pair<uint32_t, uint32_t>>;

}  // namespace

TEST_CASE("a CPU write releases only the scaled pages it covers",
          "[graphics][texture_cache][scaled]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  cache.MarkRangeAsResolved(0x100000, 0x10000);
  memory.MemoryInvalidationCallback(0x104000, 0x2000, true);
  CHECK(cache.GetScaledResolvedPageRuns() == Runs{{0x100000, 0x4000}, {0x106000, 0xA000}});
}

TEST_CASE("scaled runs split where the bytes per texel change",
          "[graphics][texture_cache][scaled]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  cache.MarkRangeAsResolved(0x200000, 0x2000, 2);
  cache.MarkRangeAsResolved(0x202000, 0x2000, 2);
  cache.MarkRangeAsResolved(0x204000, 0x1000, 3);
  cache.MarkRangeAsResolved(0x205000, 0x1000);
  const auto runs = cache.GetScaledResolveRuns();
  REQUIRE(runs.size() == 2);
  CHECK(runs[0].start == 0x200000);
  CHECK(runs[0].length == 0x4000);
  CHECK(runs[0].bpb_log2 == 2);
  CHECK(runs[1].start == 0x204000);
  CHECK(runs[1].length == 0x1000);
  CHECK(runs[1].bpb_log2 == 3);
  CHECK(cache.GetScaledResolvedPageRuns() == Runs{{0x200000, 0x6000}});
}

TEST_CASE("an uncompressed texture's base decides over a stale mip page",
          "[graphics][texture_cache][scaled]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  SetMippedFetch(regs);
  cache.RequestTextures(1);
  const auto* cpu = cache.bound();
  REQUIRE(cpu);
  REQUIRE(cpu->GetGuestMipsSize());
  const uint32_t base_size = cpu->GetGuestBaseSize();

  cache.MarkRangeAsResolved(kBase, base_size);
  cache.MarkRangeAsResolved(kMips, cpu->GetGuestMipsSize());
  cache.RequestTextures(1);
  REQUIRE(cache.bound());
  CHECK(cache.bound()->key().scaled_resolve);

  memory.MemoryInvalidationCallback(kBase, base_size, true);
  cache.RequestTextures(1);
  REQUIRE(cache.bound());
  CHECK_FALSE(cache.bound()->key().scaled_resolve);
}

TEST_CASE("a texture with its base resolved stays scaled without its mips",
          "[graphics][texture_cache][scaled]") {
  RegisterFile regs;
  TestSharedMemory memory;
  TestTextureCache cache(regs, memory);
  SetMippedFetch(regs);
  cache.RequestTextures(1);
  const auto* cpu = cache.bound();
  REQUIRE(cpu);
  REQUIRE(cpu->GetGuestMipsSize());

  cache.MarkRangeAsResolved(kBase, cpu->GetGuestBaseSize());
  cache.RequestTextures(1);
  REQUIRE(cache.bound());
  CHECK(cache.bound()->key().scaled_resolve);
}
