#pragma once
// The motion a temporal upscaler gets for FM4's spinning wheels. A wheel's own matrix turns its
// tread through tens of degrees a frame at speed (0.7 rad at 100 km/h and 120 fps), far past
// what an upscaler can follow: the tread's vectors pointed at the ground beside the car, and
// its history drew a light line along the body by the wheels. A spinning object moves with its
// hub (its mesh's origin: one wheel mesh is drawn at all four corners) instead.

#include <array>
#include <optional>

namespace rex::graphics {

// When the object spins, a reprojection (row-major, column vectors: the current frame's clip
// space to the previous frame's) moving it on the screen as its origin moves; empty when it
// does not, or when its origin is behind either frame's camera. The world-view-projections are
// row-major for column vectors (Fm4ObjectMatrix).
std::optional<std::array<double, 16>> Fm4SpinFreeReprojection(
    const std::array<float, 16>& previous_wvp, const std::array<float, 16>& current_wvp);

}  // namespace rex::graphics
