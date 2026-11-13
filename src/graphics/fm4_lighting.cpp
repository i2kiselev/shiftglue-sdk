/**
 * @file        graphics/fm4_lighting.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/graphics/fm4_lighting.h>

#include <algorithm>
#include <bit>

#include <rex/cvar.h>
#include <rex/graphics/register_file.h>

REXCVAR_DEFINE_INT32(fm4_lighting_bloom, 100, "Forza Motorsport 4",
                     "Bloom strength in percent of the track's (bloomScale)")
    .range(0, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fm4_lighting_bloom_threshold, 100, "Forza Motorsport 4",
                     "Brightness where bloom starts, in percent of the track's (bloomCutoff): "
                     "higher leaves bloom to the brightest parts")
    .range(25, 400)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fm4_lighting_exposure, 0, "Forza Motorsport 4",
                     "Exposure added to the game's, after its eye adaptation, in hundredths of "
                     "a stop (100 is twice as bright)")
    .range(-300, 300)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(fm4_lighting_saturation, 100, "Forza Motorsport 4",
                     "Colour saturation in percent of the track's (most tracks take 15% of the "
                     "colour away: 118% is full colour there)")
    .range(0, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {

namespace {

// fsr4-patch/hdr/POST_CHAIN.md.
constexpr uint64_t kBloomBrightPassPixelShader = 0xD31AE23A56F1818Dull;
constexpr uint64_t kTonemapPixelShader = 0xE97AA211B556A9B3ull;
constexpr uint64_t kTonemapVertexShader = 0x50FDB5D3AB1CC2D8ull;

uint32_t VertexConstant(uint32_t index, uint32_t component) {
  return XE_GPU_REG_SHADER_CONSTANT_000_X + 4 * index + component;
}
uint32_t PixelConstant(uint32_t index, uint32_t component) {
  return XE_GPU_REG_SHADER_CONSTANT_256_X + 4 * index + component;
}

float Read(const uint32_t* registers, uint32_t reg) {
  return std::bit_cast<float>(registers[reg]);
}

void Write(std::vector<std::pair<uint32_t, uint32_t>>& patches, uint32_t reg, float value) {
  patches.emplace_back(reg, std::bit_cast<uint32_t>(value));
}

}  // namespace

void Fm4LightingPatches(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                        const uint32_t* registers,
                        std::vector<std::pair<uint32_t, uint32_t>>& patches) {
  if (pixel_shader_hash == kBloomBrightPassPixelShader) {
    // c3.x: bloomCutoff, c3.y: bloomScale.
    const int32_t bloom = REXCVAR_GET(fm4_lighting_bloom);
    const int32_t threshold = REXCVAR_GET(fm4_lighting_bloom_threshold);
    if (threshold != 100) {
      const uint32_t reg = PixelConstant(3, 0);
      Write(patches, reg, Read(registers, reg) * float(threshold) / 100.0f);
    }
    if (bloom != 100) {
      const uint32_t reg = PixelConstant(3, 1);
      Write(patches, reg, Read(registers, reg) * float(bloom) / 100.0f);
    }
    return;
  }
  if (pixel_shader_hash != kTonemapPixelShader) {
    return;
  }
  if (vertex_shader_hash == kTonemapVertexShader) {
    // c5.w: the log2 exposure added after the eye adaptation's (filmicTone exposure).
    const int32_t exposure = REXCVAR_GET(fm4_lighting_exposure);
    if (exposure != 0) {
      const uint32_t reg = VertexConstant(5, 3);
      Write(patches, reg, Read(registers, reg) + float(exposure) / 100.0f);
    }
  }
  const int32_t saturation = REXCVAR_GET(fm4_lighting_saturation);
  if (saturation != 100) {
    // c2, c3, c4: the red, green and blue rows of k * (s * I + (1 - s) * luma): the track's
    // saturation s (DesaturateAmount), and the brightness k fades to black and from it scale
    // the matrix by. A row sums to k, the diagonal less the luma share is k * s.
    const float k = Read(registers, PixelConstant(2, 0)) + Read(registers, PixelConstant(2, 1)) +
                    Read(registers, PixelConstant(2, 2));
    if (k > 1.0e-4f) {
      const float game =
          (Read(registers, PixelConstant(2, 0)) - Read(registers, PixelConstant(3, 0))) / k;
      const float s = std::clamp(game * float(saturation) / 100.0f, 0.0f, 2.0f);
      constexpr float kLuma[3] = {0.2126f, 0.7152f, 0.0722f};
      for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column) {
          Write(patches, PixelConstant(2 + row, column),
                k * ((row == column ? s : 0.0f) + (1.0f - s) * kLuma[column]));
        }
      }
    }
  }
}

}  // namespace rex::graphics
