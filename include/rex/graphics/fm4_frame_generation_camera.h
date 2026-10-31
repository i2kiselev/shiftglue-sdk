#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace rex::graphics {

// Camera-space data for a frame generator. Matrices are row-major for column
// vectors. FM4's c128-c131 registers are the rows of P * V.
struct Fm4FrameGenerationCamera {
  std::array<float, 16> view_to_clip;
  std::array<float, 16> clip_to_view;
  std::array<float, 16> clip_to_previous;
  std::array<float, 16> previous_to_clip;
  std::array<float, 3> position;
  std::array<float, 3> right;
  std::array<float, 3> up;
  std::array<float, 3> forward;
};

// Reconstructs the D3D projection from its vertical lens and the horizontal
// scale already present in the VP matrix, then factors P^-1 * (P * V). The row
// basis determines the projection's Y sign, which is negative in the title's
// turned-clip scenes. Empty when the input is not a valid projective camera or
// the recovered view basis is degenerate.
inline std::optional<Fm4FrameGenerationCamera> Fm4FactorFrameGenerationCamera(
    const std::array<float, 16>& world_view_projection, float fov_y, float near_plane,
    float far_plane) {
  if (!(fov_y > 0.0f) || !(near_plane > 0.0f) || !std::isfinite(fov_y) ||
      !std::isfinite(near_plane) || !std::isfinite(far_plane) ||
      !std::all_of(world_view_projection.begin(), world_view_projection.end(),
                   [](float value) { return std::isfinite(value); })) {
    return std::nullopt;
  }

  using Matrix = std::array<double, 16>;
  const auto multiply = [](const Matrix& a, const Matrix& b) {
    Matrix out = {};
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        for (int k = 0; k < 4; ++k)
          out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
      }
    }
    return out;
  };
  const auto invert = [](const Matrix& m, Matrix& out) {
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
        if (std::abs(a[r][col]) > std::abs(a[pivot][col]))
          pivot = r;
      }
      if (std::abs(a[pivot][col]) < 1.0e-12)
        return false;
      if (pivot != col) {
        for (int c = 0; c < 8; ++c)
          std::swap(a[pivot][c], a[col][c]);
      }
      const double reciprocal = 1.0 / a[col][col];
      for (int c = 0; c < 8; ++c)
        a[col][c] *= reciprocal;
      for (int r = 0; r < 4; ++r) {
        if (r == col)
          continue;
        const double factor = a[r][col];
        if (factor == 0.0)
          continue;
        for (int c = 0; c < 8; ++c)
          a[r][c] -= factor * a[col][c];
      }
    }
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c)
        out[r * 4 + c] = a[r][c + 4];
    }
    return true;
  };

  const double scale_y_sign = (world_view_projection[1] * world_view_projection[6] -
                               world_view_projection[2] * world_view_projection[5]) *
                                  world_view_projection[12] +
                              (world_view_projection[2] * world_view_projection[4] -
                               world_view_projection[0] * world_view_projection[6]) *
                                  world_view_projection[13] +
                              (world_view_projection[0] * world_view_projection[5] -
                               world_view_projection[1] * world_view_projection[4]) *
                                  world_view_projection[14];
  if (std::abs(scale_y_sign) < 1.0e-12)
    return std::nullopt;

  const double scale = 1.0 / std::tan(0.5 * double(fov_y));
  const double scale_x = std::sqrt(double(world_view_projection[0]) * world_view_projection[0] +
                                   double(world_view_projection[1]) * world_view_projection[1] +
                                   double(world_view_projection[2]) * world_view_projection[2]);
  const double scale_y = std::copysign(scale, scale_y_sign);
  const bool finite_far = far_plane > near_plane;
  const double depth_scale =
      finite_far ? double(far_plane) / (double(far_plane) - near_plane) : 1.0;
  const double depth_bias = finite_far
                                ? -double(near_plane) * far_plane / (double(far_plane) - near_plane)
                                : -double(near_plane);
  const Matrix projection = {scale_x, 0.0, 0.0,         0.0,        0.0, scale_y, 0.0, 0.0,
                             0.0,     0.0, depth_scale, depth_bias, 0.0, 0.0,     1.0, 0.0};
  Matrix projection_inverse;
  if (!invert(projection, projection_inverse))
    return std::nullopt;

  Matrix vp;
  for (size_t i = 0; i < 16; ++i)
    vp[i] = world_view_projection[i];
  const Matrix view = multiply(projection_inverse, vp);
  const double right_length = std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
  const double up_length = std::sqrt(view[4] * view[4] + view[5] * view[5] + view[6] * view[6]);
  const double forward_length =
      std::sqrt(view[8] * view[8] + view[9] * view[9] + view[10] * view[10]);
  if (!(right_length > 1.0e-12) || !(up_length > 1.0e-12) || !(forward_length > 1.0e-12) ||
      !std::isfinite(right_length) || !std::isfinite(up_length) || !std::isfinite(forward_length)) {
    return std::nullopt;
  }
  const auto close_to_unit = [](double value) { return std::abs(value - 1.0) <= 1.0e-3; };
  const auto near_zero = [](double value) { return std::abs(value) <= 1.0e-3; };
  const auto dot_rows = [&](int a, int b) {
    return view[a * 4] * view[b * 4] + view[a * 4 + 1] * view[b * 4 + 1] +
           view[a * 4 + 2] * view[b * 4 + 2];
  };
  if (!close_to_unit(right_length) || !close_to_unit(up_length) || !close_to_unit(forward_length) ||
      !near_zero(dot_rows(0, 1) / (right_length * up_length)) ||
      !near_zero(dot_rows(0, 2) / (right_length * forward_length)) ||
      !near_zero(dot_rows(1, 2) / (up_length * forward_length)) || !near_zero(view[12]) ||
      !near_zero(view[13]) || !near_zero(view[14]) || !close_to_unit(view[15])) {
    return std::nullopt;
  }

  // FM4 uses D3D's left-handed clip convention. Reject a factor with a
  // reflected or otherwise invalid camera basis rather than handing providers
  // a plausible-looking but false transform.
  const double handedness = (view[1] * view[6] - view[2] * view[5]) * view[8] +
                            (view[2] * view[4] - view[0] * view[6]) * view[9] +
                            (view[0] * view[5] - view[1] * view[4]) * view[10];
  if (!(handedness > 0.0) || !std::isfinite(handedness))
    return std::nullopt;

  Fm4FrameGenerationCamera camera{};
  for (size_t i = 0; i < 16; ++i) {
    camera.view_to_clip[i] = float(projection[i]);
    camera.clip_to_view[i] = float(projection_inverse[i]);
  }
  for (int i = 0; i < 3; ++i) {
    camera.right[i] = float(view[i] / right_length);
    camera.up[i] = float(view[4 + i] / up_length);
    camera.forward[i] = float(view[8 + i] / forward_length);
    camera.position[i] =
        float(-(view[i] * view[3] + view[4 + i] * view[7] + view[8 + i] * view[11]));
  }
  return camera;
}

}  // namespace rex::graphics
