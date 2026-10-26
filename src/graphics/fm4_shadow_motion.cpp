#include <rex/graphics/fm4_shadow_motion.h>

#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace rex::graphics {

namespace {

using Matrix = std::array<double, 16>;

Matrix ToMatrix(const std::array<float, 16>& m) {
  Matrix out;
  for (size_t i = 0; i < 16; ++i) out[i] = m[i];
  return out;
}

Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out = {};
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = sum;
    }
  }
  return out;
}

// Gauss-Jordan with partial pivoting.
std::optional<Matrix> Invert(const Matrix& m) {
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][c + 4] = r == c ? 1.0 : 0.0;
    }
  }
  for (int col = 0; col < 4; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 4; ++r) {
      if (std::abs(a[r][col]) > std::abs(a[pivot][col])) pivot = r;
    }
    if (std::abs(a[pivot][col]) < 1e-12) return std::nullopt;
    if (pivot != col) {
      for (int c = 0; c < 8; ++c) std::swap(a[pivot][c], a[col][c]);
    }
    const double inv = 1.0 / a[col][col];
    for (int c = 0; c < 8; ++c) a[col][c] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == col) continue;
      const double f = a[r][col];
      if (f == 0.0) continue;
      for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
    }
  }
  Matrix out;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  }
  return out;
}

}  // namespace

bool Fm4OrthographicMatrix(const std::array<float, 16>& wvp) {
  return wvp[12] == 0.0f && wvp[13] == 0.0f && wvp[14] == 0.0f && wvp[15] == 1.0f;
}

std::optional<std::array<double, 16>> Fm4ShadowToScene(
    const std::vector<std::pair<std::array<float, 16>, std::array<float, 16>>>& pairs) {
  // Equal up to the float matrices' rounding: 1/256 of clip space.
  std::unordered_map<uint64_t, std::pair<size_t, Matrix>> groups;
  const std::pair<size_t, Matrix>* largest = nullptr;
  for (const auto& [scene, shadow] : pairs) {
    const std::optional<Matrix> shadow_inverse = Invert(ToMatrix(shadow));
    if (!shadow_inverse) continue;
    const Matrix product = Multiply(ToMatrix(scene), *shadow_inverse);
    uint64_t hash = 1469598103934665603ull;
    for (double element : product) {
      hash ^= uint64_t(std::llround(element * 256.0));
      hash *= 1099511628211ull;
    }
    auto& group = groups.try_emplace(hash, 0, product).first->second;
    ++group.first;
    if (!largest || group.first > largest->first) largest = &group;
  }
  if (!largest) return std::nullopt;
  return largest->second;
}

std::optional<std::array<double, 16>> Fm4ShadowReprojection(
    const std::array<double, 16>& previous_shadow_to_scene,
    const std::array<double, 16>& current_shadow_to_scene) {
  const std::optional<Matrix> current_inverse = Invert(current_shadow_to_scene);
  if (!current_inverse) return std::nullopt;
  return Multiply(previous_shadow_to_scene, *current_inverse);
}

}  // namespace rex::graphics
