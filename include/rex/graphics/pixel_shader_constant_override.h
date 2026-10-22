#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace rex::graphics {

// Float constant components of one pixel shader replaced in each of its draws
// (gpu_pixel_shader_constant_override).
struct PixelShaderConstantOverride {
  uint64_t shader_hash = 0;
  uint32_t constant = 0;
  // Bit i is component i: x = 1, y = 2, z = 4, w = 8.
  uint32_t component_mask = 0xF;
  // Only the masked components are meaningful.
  std::array<float, 4> value{};
};

// "" is no override. Entries are separated by ';', each
// <hex hash>:c<n>=<x>,<y>,<z>,<w> or <hex hash>:c<n>.<components>=<one value per component>,
// the components a subset of xyzw in that order. One bad entry rejects the whole text.
std::optional<std::vector<PixelShaderConstantOverride>> ParsePixelShaderConstantOverrides(
    std::string_view text);

// Writes the masked components of the override into a constant's four registers.
void ApplyConstantOverride(const PixelShaderConstantOverride& constant_override,
                           std::array<uint32_t, 4>& registers);

}  // namespace rex::graphics
