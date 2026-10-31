/**
 * @file        rex/graphics/pipeline/shader/wrap_seam.h
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace rex::graphics {

// Shaders whose interpolator 0 z is an angle that wraps from 1 to 0 across some triangles
// (gpu_wrap_seam_*): with the fix, the vertex shader also writes frac(z + 0.5), and the pixel
// shader's fetches at r0.z take whichever of the two copies varies less over the quad, so the
// wrap no longer interpolates through the whole texture.
struct WrapSeamShaders {
  std::unordered_set<uint64_t> vertex;
  std::unordered_set<uint64_t> pixel;
};

// Comma-separated hexadecimal hashes, spaces allowed; "" is none. A bad token rejects the list.
std::optional<std::unordered_set<uint64_t>> ParseShaderHashList(std::string_view text);

struct WrapSeamBits {
  bool vertex = false;
  bool pixel = false;
};

// pixel_shader_hash 0 is no pixel shader. The pixel bit needs the vertex shader listed too, as
// only a listed vertex shader writes the second copy. A draw with point parameters (point
// coordinates or size) gets no bits: the second copy's location would follow them in one stage
// and not in the other.
WrapSeamBits SelectWrapSeamBits(const WrapSeamShaders& shaders, bool fix,
                                uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                                bool point_parameters);

}  // namespace rex::graphics
