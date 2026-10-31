/**
 * @file        rex/graphics/fm4_camera_cut.h
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
// Whether a temporal upscaler's history carries over between two frames, from how far the
// camera's motion moves the picture on the screen. The reprojection matrix's raw entries
// scale a camera translation by 1 / near: FM4's chase and hood cameras (near 0.1-0.3) read
// an ordinary sideways drift in a corner as a cut, and races dropped the history on up to
// nine frames in ten.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace rex::graphics {

// A shift of an eighth of the screen in one frame or more is a cut. Below it the matched
// world's matrices give exact motion, which the upscaler follows without its history going
// stale (driving at 300 km/h at 60 fps shifts the corners by about 0.05).
inline constexpr double kFm4CameraCutScreenShift = 0.25;

// The largest shift, in normalised device coordinates, of the screen's centre and corners
// at view depth sqrt(near * far) when `reprojection` (row-major, column vectors: the
// current frame's clip space to the previous frame's) carries them into the previous
// frame; infinity when one of them lands behind the previous camera.
double Fm4ReprojectedScreenShift(const std::array<double, 16>& reprojection, double near_plane,
                                 double far_plane);

// The object slot whose reprojection the most scene draws share: the camera's motion where the
// camera's matrix registers hold no projection. FM4's showroom, pre-race and Autovista place
// every mesh by its own matrix, and every static mesh reprojects as the camera does, so the
// history was dropped on every frame there. 0 when fewer than min_draws share one;
// slot_draws[0] counts the draws without a slot.
uint32_t Fm4CameraObjectSlot(const std::vector<uint32_t>& slot_draws, uint32_t min_draws);

// A depth surface the camera's matrix draws tested against in a frame.
struct Fm4DepthCandidate {
  uint32_t key;
  // Guest pixels.
  uint32_t width;
  size_t draws;
};

// The scene's depth among a frame's candidates: the widest, then the most draws. FM4's
// replays render the car's reflection cube (280 pixels wide) with more camera draws than a
// long-lens replay camera's scene (1280), and picking by draws alone made the cube's 90-degree
// face the camera on most replay frames: a cut each time, its depth and the jitter's sign
// handed to the upscaler. 0 when there are none.
uint32_t Fm4SceneDepth(const std::vector<Fm4DepthCandidate>& candidates);

// An object's world-view-projection, row-major for column vectors as the camera's c128-c131
// are, from its c0-c3. FM4's object shaders take the registers as columns (x * c0 + y * c1 +
// z * c2 + c3); read as rows, every moving car's reprojection was wrong.
std::array<float, 16> Fm4ObjectMatrix(const std::array<float, 16>& registers);

// The first register of a vertex shader's world-view-projection, from the float constants it
// reads: 128 for the camera's (the world), 0 for an object's own, UINT32_MAX for neither. A
// shader that indexes its constants (the driver's bone palette) is marked as reading all 256,
// and read as a world draw it took bones for the camera's matrix: the driver got the camera's
// motion and ghosted behind the windscreen. Such shaders place their mesh by c0-c3.
uint32_t Fm4MatrixRegister(const std::array<uint64_t, 4>& float_bitmap, bool dynamic_addressing);

// Whether a draw of a shader that reads both register groups is placed by its c0-c3 although
// it reads c128-c131: those hold no matrix (a singular one). The 599 GTO's exhaust tips read
// them as constants and their offset; taken for world draws, their reprojection failed and
// they got the camera's motion, the asphalt's, so a car driving across the screen fizzled
// at its tail.
bool Fm4PlacedByObjectMatrix(const std::array<uint64_t, 4>& float_bitmap,
                             const std::array<float, 16>& camera_registers);

// Whether a world-view-projection (row-major for column vectors, y up) draws the scene turned
// 180 degrees in clip space: world up landing on clip -y. Races draw it so and the post pass
// turns it back; the showroom draws it upright, and treating it as turned handed the upscaler
// its depth and motion upside down and the jitter negated. Empty when the screen's vertical
// is mostly not world up (a camera rolled on its side, or looking down), which cannot tell.
std::optional<bool> Fm4SceneTurned(const std::array<float, 16>& rows);

}  // namespace rex::graphics
