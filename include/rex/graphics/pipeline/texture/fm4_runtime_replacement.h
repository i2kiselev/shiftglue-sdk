/**
 * @file        rex/graphics/pipeline/texture/fm4_runtime_replacement.h
 *
 * @brief       Texture replacements an app supplies at run time (FM4).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics::texture_replacement {

// An image that replaces the guest texture whose base level hashes to a
// registered hash, under the same rules as a <hash>.dds file. It lives in
// rexruntime so the app can register images the GPU plugin then reads.
struct RuntimeImage {
  xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
  uint32_t width = 0, height = 0, levels = 0;
  std::vector<uint8_t> data;  // levels in order, blocks row by row
};

// nullptr removes the replacement. Thread-safe.
void SetRuntimeReplacement(uint64_t hash, std::shared_ptr<const RuntimeImage> image);
std::shared_ptr<const RuntimeImage> FindRuntimeReplacement(uint64_t hash);
bool HasRuntimeReplacements();

// Whether a texture of this format and size could be replaced by a registered
// image (the same format, 1x, 2x, 4x or 8x the size), so caches hash only
// textures that may be.
bool RuntimeReplacementMayMatch(xenos::TextureFormat format, uint32_t width, uint32_t height);

// Increases on every Set that changes something.
uint64_t RuntimeGeneration();

// The hashes changed since the last call; the texture cache drains it.
std::vector<uint64_t> TakeChangedRuntimeHashes();

}  // namespace rex::graphics::texture_replacement
