#include <rex/graphics/pipeline/shader/wrap_seam.h>

#include <algorithm>
#include <charconv>

namespace rex::graphics {

std::optional<std::unordered_set<uint64_t>> ParseShaderHashList(std::string_view text) {
  std::unordered_set<uint64_t> hashes;
  if (text.find_first_not_of(' ') == std::string_view::npos) {
    return hashes;
  }
  while (true) {
    const size_t comma = text.find(',');
    std::string_view token = text.substr(0, comma);
    token.remove_prefix(std::min(token.find_first_not_of(' '), token.size()));
    token.remove_suffix(token.size() - std::min(token.find_last_not_of(' ') + 1, token.size()));
    uint64_t hash = 0;
    const char* end = token.data() + token.size();
    const std::from_chars_result result = std::from_chars(token.data(), end, hash, 16);
    if (token.empty() || result.ec != std::errc() || result.ptr != end) {
      return std::nullopt;
    }
    hashes.insert(hash);
    if (comma == std::string_view::npos) {
      return hashes;
    }
    text.remove_prefix(comma + 1);
  }
}

WrapSeamBits SelectWrapSeamBits(const WrapSeamShaders& shaders, bool fix,
                                uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                                bool point_parameters) {
  WrapSeamBits bits;
  bits.vertex = fix && !point_parameters && shaders.vertex.count(vertex_shader_hash);
  bits.pixel = bits.vertex && pixel_shader_hash && shaders.pixel.count(pixel_shader_hash);
  return bits;
}

}  // namespace rex::graphics
