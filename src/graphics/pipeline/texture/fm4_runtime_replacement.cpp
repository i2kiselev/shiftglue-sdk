/**
 * @file        graphics/pipeline/texture/fm4_runtime_replacement.cpp
 *
 * @brief       Texture replacements an app supplies at run time (FM4).
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/pipeline/texture/fm4_runtime_replacement.h>

#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace rex::graphics::texture_replacement {

namespace {

struct RuntimeState {
  std::mutex mutex;
  std::unordered_map<uint64_t, std::shared_ptr<const RuntimeImage>> images;
  std::unordered_set<uint64_t> changed;
  uint64_t generation = 0;
};

RuntimeState& GetRuntimeState() {
  static RuntimeState state;
  return state;
}

}  // namespace

void SetRuntimeReplacement(uint64_t hash, std::shared_ptr<const RuntimeImage> image) {
  auto& state = GetRuntimeState();
  std::lock_guard lock(state.mutex);
  const auto found = state.images.find(hash);
  if (!image) {
    if (found == state.images.end())
      return;
    state.images.erase(found);
  } else if (found == state.images.end()) {
    state.images.emplace(hash, std::move(image));
  } else if (found->second == image) {
    return;
  } else {
    found->second = std::move(image);
  }
  state.changed.insert(hash);
  ++state.generation;
}

std::shared_ptr<const RuntimeImage> FindRuntimeReplacement(uint64_t hash) {
  auto& state = GetRuntimeState();
  std::lock_guard lock(state.mutex);
  const auto found = state.images.find(hash);
  return found == state.images.end() ? nullptr : found->second;
}

bool HasRuntimeReplacements() {
  auto& state = GetRuntimeState();
  std::lock_guard lock(state.mutex);
  return !state.images.empty();
}

bool RuntimeReplacementMayMatch(xenos::TextureFormat format, uint32_t width, uint32_t height) {
  auto& state = GetRuntimeState();
  std::lock_guard lock(state.mutex);
  for (const auto& [hash, image] : state.images) {
    if (image->format != format)
      continue;
    for (uint32_t scale_log2 = 0; scale_log2 <= 3; ++scale_log2) {
      if (image->width == (width << scale_log2) && image->height == (height << scale_log2)) {
        return true;
      }
    }
  }
  return false;
}

uint64_t RuntimeGeneration() {
  auto& state = GetRuntimeState();
  std::lock_guard lock(state.mutex);
  return state.generation;
}

std::vector<uint64_t> TakeChangedRuntimeHashes() {
  auto& state = GetRuntimeState();
  std::lock_guard lock(state.mutex);
  std::vector<uint64_t> changed(state.changed.begin(), state.changed.end());
  state.changed.clear();
  return changed;
}

}  // namespace rex::graphics::texture_replacement
