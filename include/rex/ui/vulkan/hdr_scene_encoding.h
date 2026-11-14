/**
 * @file        rex/ui/vulkan/hdr_scene_encoding.h
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
// present_hdr_native with a temporal upscaler: the HUD-less guest output carries the HDR scene
// (fh1_hdr_gain.cpp EncodeHudlessHdr) so the upscaler works on the HDR picture, in its 10-bit
// format. Per channel, in the display-linear brightness the HDR pass decodes (1 = paper white):
// up to the knee the value is the SDR picture's own, so SDR content in it (the mirror glass,
// the HUD's blends) decodes as before; above it, up to the peak, a logarithm whose slope
// meets the straight part's at the knee, for an even share of codes per stop (about 0.8 % per
// code at a 5x peak, as fine as HDR10's PQ). hdr_output.ps.glsl decodes it.

#pragma once

#include <algorithm>
#include <cmath>

namespace rex::ui::vulkan {

struct HdrSceneEncoding {
  // Where the logarithm starts, the peak (both over paper white) and the logarithm's scale;
  // a peak of 1 or less leaves the encoding straight (clamped at 1).
  float knee = 0.5f;
  float peak = 1.0f;
  float log_scale = 0.0f;
};

inline HdrSceneEncoding MakeHdrSceneEncoding(double peak_nits, double paper_white_nits) {
  HdrSceneEncoding encoding;
  encoding.peak = float(std::max(peak_nits / std::max(paper_white_nits, 1.0), 1.0));
  if (encoding.peak <= 1.01f) {
    encoding.peak = 1.0f;
    return encoding;
  }
  // (1 - k) L = ln(1 + (P - k) L): the logarithm's slope at the knee is 1.
  const double k = encoding.knee, p = encoding.peak;
  double low = 1.0e-6, high = 1.0e5;
  for (int i = 0; i < 200; ++i) {
    const double middle = std::sqrt(low * high);
    if ((1.0 - k) * middle < std::log1p((p - k) * middle)) {
      low = middle;
    } else {
      high = middle;
    }
  }
  encoding.log_scale = float(std::sqrt(low * high));
  return encoding;
}

}  // namespace rex::ui::vulkan
