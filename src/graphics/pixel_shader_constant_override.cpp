#include <rex/graphics/pixel_shader_constant_override.h>

#include <bit>
#include <charconv>

namespace rex::graphics {

namespace {

template <typename T>
bool ParseWhole(std::string_view text, T& value, int base = 10) {
  const char* end = text.data() + text.size();
  std::from_chars_result result;
  if constexpr (std::is_floating_point_v<T>) {
    result = std::from_chars(text.data(), end, value);
  } else {
    result = std::from_chars(text.data(), end, value, base);
  }
  return !text.empty() && result.ec == std::errc() && result.ptr == end;
}

std::optional<PixelShaderConstantOverride> ParseEntry(std::string_view entry) {
  PixelShaderConstantOverride parsed;
  const size_t colon = entry.find(':');
  const size_t equals = entry.find('=');
  if (colon == std::string_view::npos || equals == std::string_view::npos || equals < colon ||
      !ParseWhole(entry.substr(0, colon), parsed.shader_hash, 16)) {
    return std::nullopt;
  }
  std::string_view target = entry.substr(colon + 1, equals - colon - 1);
  if (target.empty() || target.front() != 'c') {
    return std::nullopt;
  }
  target.remove_prefix(1);
  const size_t dot = target.find('.');
  if (!ParseWhole(target.substr(0, dot), parsed.constant) || parsed.constant > 255) {
    return std::nullopt;
  }
  if (dot != std::string_view::npos) {
    const std::string_view components = target.substr(dot + 1);
    parsed.component_mask = 0;
    int last = -1;
    for (const char component : components) {
      const size_t index = std::string_view("xyzw").find(component);
      if (index == std::string_view::npos || int(index) <= last) {
        return std::nullopt;
      }
      last = int(index);
      parsed.component_mask |= 1u << index;
    }
    if (!parsed.component_mask) {
      return std::nullopt;
    }
  }
  std::string_view values = entry.substr(equals + 1);
  int remaining = std::popcount(parsed.component_mask);
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(parsed.component_mask & (1u << i))) {
      continue;
    }
    const size_t comma = values.find(',');
    if ((comma == std::string_view::npos) != (--remaining == 0) ||
        !ParseWhole(values.substr(0, comma), parsed.value[i])) {
      return std::nullopt;
    }
    values.remove_prefix(comma == std::string_view::npos ? values.size() : comma + 1);
  }
  return parsed;
}

}  // namespace

std::optional<std::vector<PixelShaderConstantOverride>> ParsePixelShaderConstantOverrides(
    std::string_view text) {
  std::vector<PixelShaderConstantOverride> overrides;
  if (text.empty()) {
    return overrides;
  }
  while (true) {
    const size_t separator = text.find(';');
    const std::optional<PixelShaderConstantOverride> entry = ParseEntry(text.substr(0, separator));
    if (!entry) {
      return std::nullopt;
    }
    overrides.push_back(*entry);
    if (separator == std::string_view::npos) {
      return overrides;
    }
    text.remove_prefix(separator + 1);
  }
}

void ApplyConstantOverride(const PixelShaderConstantOverride& constant_override,
                           std::array<uint32_t, 4>& registers) {
  for (uint32_t i = 0; i < 4; ++i) {
    if (constant_override.component_mask & (1u << i)) {
      registers[i] = std::bit_cast<uint32_t>(constant_override.value[i]);
    }
  }
}

}  // namespace rex::graphics
