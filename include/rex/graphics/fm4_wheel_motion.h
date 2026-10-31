/**
 * @file        rex/graphics/fm4_wheel_motion.h
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
// The motion a temporal upscaler gets for FM4's spinning wheels. A wheel's own matrix turns its
// tread through tens of degrees a frame at speed (0.7 rad at 100 km/h and 120 fps), far past
// what an upscaler can follow: the tread's vectors pointed at the ground beside the car, and
// its history drew a light line along the body by the wheels. A spinning wheel keeps all of its
// motion but the spin: its hub's, its steering's and its car's turn.

#include <array>
#include <optional>

namespace rex::graphics {

// When the object spins about its x axis (a wheel's axle) between the two frames' world
// matrices (c8-c11 in the car layout), the previous frame's world-view-projection turned by
// this frame's spin; empty when it does not. All matrices are row-major for column vectors
// (Fm4ObjectMatrix). The spin is measured in the object's own space: on the screen a wheel seen
// almost edge-on hid it every other frame.
std::optional<std::array<double, 16>> Fm4SpinFreePreviousWvp(
    const std::array<float, 16>& previous_wvp, const std::array<float, 16>& previous_world,
    const std::array<float, 16>& current_world);

}  // namespace rex::graphics
