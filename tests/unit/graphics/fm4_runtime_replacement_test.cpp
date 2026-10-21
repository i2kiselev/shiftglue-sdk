#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>

#include <rex/graphics/pipeline/texture/fm4_runtime_replacement.h>

namespace tr = rex::graphics::texture_replacement;

namespace {

std::shared_ptr<const tr::RuntimeImage> MakeImage(uint32_t size) {
  auto image = std::make_shared<tr::RuntimeImage>();
  image->format = rex::graphics::xenos::TextureFormat::k_8_8_8_8;
  image->width = size;
  image->height = size;
  image->levels = 1;
  image->data.assign(size_t(size) * size * 4, 0xFF);
  return image;
}

// The registry is process-wide; each test starts from an empty, drained one.
void Clear(std::initializer_list<uint64_t> hashes) {
  for (uint64_t hash : hashes)
    tr::SetRuntimeReplacement(hash, nullptr);
  tr::TakeChangedRuntimeHashes();
}

}  // namespace

TEST_CASE("Run-time replacements are taken once after they change", "[fm4_runtime_replacement]") {
  Clear({1, 2});
  const uint64_t generation = tr::RuntimeGeneration();
  tr::SetRuntimeReplacement(1, MakeImage(32));
  tr::SetRuntimeReplacement(2, MakeImage(32));
  auto changed = tr::TakeChangedRuntimeHashes();
  std::sort(changed.begin(), changed.end());
  CHECK(changed == std::vector<uint64_t>{1, 2});
  CHECK(tr::TakeChangedRuntimeHashes().empty());
  CHECK(tr::RuntimeGeneration() == generation + 2);
  CHECK(tr::FindRuntimeReplacement(1) != nullptr);
  Clear({1, 2});
}

TEST_CASE("Removing a run-time replacement is a change", "[fm4_runtime_replacement]") {
  Clear({3});
  tr::SetRuntimeReplacement(3, MakeImage(32));
  tr::TakeChangedRuntimeHashes();
  const uint64_t generation = tr::RuntimeGeneration();
  tr::SetRuntimeReplacement(3, nullptr);
  CHECK(tr::TakeChangedRuntimeHashes() == std::vector<uint64_t>{3});
  CHECK(tr::RuntimeGeneration() == generation + 1);
  CHECK(tr::FindRuntimeReplacement(3) == nullptr);
  CHECK_FALSE(tr::HasRuntimeReplacements());
}

TEST_CASE("Setting the same image twice is one change", "[fm4_runtime_replacement]") {
  Clear({4});
  const uint64_t generation = tr::RuntimeGeneration();
  const auto image = MakeImage(32);
  tr::SetRuntimeReplacement(4, image);
  tr::SetRuntimeReplacement(4, image);
  CHECK(tr::TakeChangedRuntimeHashes() == std::vector<uint64_t>{4});
  CHECK(tr::RuntimeGeneration() == generation + 1);
  tr::SetRuntimeReplacement(5, nullptr);
  CHECK(tr::TakeChangedRuntimeHashes().empty());
  Clear({4});
}

TEST_CASE("Only textures of a registered image's format and size may match",
          "[fm4_runtime_replacement]") {
  using rex::graphics::xenos::TextureFormat;
  Clear({6});
  CHECK_FALSE(tr::RuntimeReplacementMayMatch(TextureFormat::k_8_8_8_8, 32, 32));
  tr::SetRuntimeReplacement(6, MakeImage(128));
  CHECK(tr::RuntimeReplacementMayMatch(TextureFormat::k_8_8_8_8, 32, 32));
  CHECK(tr::RuntimeReplacementMayMatch(TextureFormat::k_8_8_8_8, 128, 128));
  CHECK_FALSE(tr::RuntimeReplacementMayMatch(TextureFormat::k_8_8_8_8, 32, 16));
  CHECK_FALSE(tr::RuntimeReplacementMayMatch(TextureFormat::k_8_8_8_8, 8, 8));
  CHECK_FALSE(tr::RuntimeReplacementMayMatch(TextureFormat::k_DXT1, 32, 32));
  Clear({6});
}
