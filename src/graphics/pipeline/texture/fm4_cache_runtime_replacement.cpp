/**
 * @file        graphics/pipeline/texture/fm4_cache_runtime_replacement.cpp
 *
 * @brief       FM4 run-time texture replacements in the texture cache.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/pipeline/texture/cache.h>

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/fm4_runtime_replacement.h>
#include <rex/graphics/pipeline/texture/replacement.h>
#include <rex/graphics/shared_memory.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(fm4_runtime_replacement_probe, "", "GPU",
                      "Diagnostics: replace the texture with this 16-digit base level hash by a "
                      "magenta 128x128 run-time image (4x a 32x32 icon); empty removes it");

namespace rex::graphics {

namespace {

std::shared_ptr<const texture_replacement::RuntimeImage> MakeProbeImage() {
  auto image = std::make_shared<texture_replacement::RuntimeImage>();
  image->format = xenos::TextureFormat::k_8_8_8_8;
  image->width = image->height = 128;
  image->levels = 8;
  for (uint32_t level = 0; level < image->levels; ++level) {
    const uint32_t size = 128u >> level;
    for (uint32_t texel = 0; texel < size * size; ++texel) {
      image->data.insert(image->data.end(), {0xFF, 0x00, 0xFF, 0xFF});
    }
  }
  return image;
}

void UpdateProbe() {
  static std::string applied;
  const std::string probe = REXCVAR_GET(fm4_runtime_replacement_probe);
  if (probe == applied)
    return;
  unsigned long long hash = 0;
  if (!applied.empty() && std::sscanf(applied.c_str(), "%16llx", &hash) == 1) {
    texture_replacement::SetRuntimeReplacement(hash, nullptr);
  }
  applied = probe;
  if (!probe.empty() && std::sscanf(probe.c_str(), "%16llx", &hash) == 1) {
    texture_replacement::SetRuntimeReplacement(hash, MakeProbeImage());
  }
  REXGPU_INFO("Run-time replacement probe: {}", probe.empty() ? "off" : probe);
}

}  // namespace

void TextureCache::Fm4ApplyRuntimeReplacements() {
  UpdateProbe();
  const std::vector<uint64_t> changed = texture_replacement::TakeChangedRuntimeHashes();
  if (changed.empty())
    return;
  const std::unordered_set<uint64_t> hashes(changed.begin(), changed.end());
  auto global_lock = global_critical_region_.Acquire();
  uint32_t reloaded = 0;
  for (auto& [key, texture] : textures_) {
    const uint64_t hash = texture->fm4_replacement_hash();
    bool reload = hash && hashes.contains(hash);
    // Loaded while no run-time image could match, so never hashed: the
    // reload hashes it.
    if (!hash && !key.scaled_resolve) {
      const texture_replacement::Format* format = texture_replacement::FindFormat(key.format);
      reload = format && texture_replacement::RuntimeReplacementMayMatch(
                             format->guest, key.GetWidth(), key.GetHeight());
    }
    if (reload) {
      texture->Fm4MarkOutdated(global_lock);
      ++reloaded;
    }
  }
  if (reloaded) {
    texture_became_outdated_.store(true, std::memory_order_release);
  }
  REXGPU_INFO("Run-time replacements: {} changed, {} textures reload", changed.size(), reloaded);
}

void TextureCache::Texture::Fm4MarkOutdated(
    const std::unique_lock<std::recursive_mutex>& global_lock) {
  SharedMemory& shared_memory = texture_cache().shared_memory();
  if (GetGuestBaseSize()) {
    if (base_watch_handle_)
      shared_memory.UnwatchMemoryRange(base_watch_handle_);
    WatchCallback(global_lock, false, false);
  }
  if (GetGuestMipsSize()) {
    if (mips_watch_handle_)
      shared_memory.UnwatchMemoryRange(mips_watch_handle_);
    WatchCallback(global_lock, true, false);
  }
}

}  // namespace rex::graphics
