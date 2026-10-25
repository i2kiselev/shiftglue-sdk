#pragma once
// Which of FM4's draws a temporal upscaler's jitter moves. The upscaler takes the whole frame
// as jittered, so a scene draw left still shakes by the jitter against the rest. FM4 draws
// its scene into one EDRAM surface through several formats, and a car's layers over its
// first pass test the depth without writing it; matching only the scene's most used colour
// format and the cars' depth-writing passes left half the cars' draws and the showroom
// floor still.

#include <cstdint>

namespace rex::graphics {

// How a vertex shader places its draw, from the float constants it reads: by the camera's
// world-view-projection (c128-c131), by an object's own (c0-c3, or c0, c1 and c3 for a flat
// mesh, whose z column is never read), or not by a matrix (screen-space passes).
enum class Fm4DrawPlacement { kUnplaced, kCamera, kObject };
Fm4DrawPlacement Fm4JitterPlacement(const uint64_t (&float_bitmap)[4]);

// Whether two colour keys (base | pitch << 16 | msaa << 32 | format << 40) name one EDRAM
// surface, whatever format each draw views it through.
bool Fm4SameColorSurface(uint64_t a, uint64_t b);

// Whether a draw is jittered: placed by a matrix, and testing the scene's depth or writing
// the scene's colour surface.
bool Fm4JitterSceneDraw(Fm4DrawPlacement placement, bool tests_scene_depth,
                        bool writes_scene_color);

}  // namespace rex::graphics
