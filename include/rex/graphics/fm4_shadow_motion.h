#pragma once
// The motion a temporal upscaler gets for FM4's car ground shadow. The track draws darken the
// ground under the car from the grey map, downsampled from the car's own shadow map, which the
// game draws again from the car every frame. The shadow therefore moves with the car, while the
// asphalt it falls on moves with the world. Given the asphalt's motion, a shadow pixel's history
// came from where the asphalt was a frame earlier; in reverse look that is the lit ground behind
// the shadow's edge, which DLSS kept as thin light lines across the shadow.
// The shadow map's own frame is no stand-in for the car's: it follows the car's position but
// keeps the sun's orientation, so in a turn it gave the shadow under the bumper 1-9 pixels of
// motion the car did not have, and DLSS ghosted the exhaust pipes into it.

#include <array>
#include <cstdint>
#include <vector>

namespace rex::graphics {

// Whether a world-view-projection (row-major for column vectors, Fm4ObjectMatrix) is
// orthographic: its w row is (0, 0, 0, 1). The car's shadow map pass draws the car so; the scene
// and the reflection cube draw it in perspective.
bool Fm4OrthographicMatrix(const std::array<float, 16>& wvp);

// The object slot the ground shadow moves with: of the scene slots of the car's meshes drawn in
// both the shadow map pass and the scene, the one most share (the body's); 0 without one. Slot 0
// (the camera's motion, meshes new this frame) does not vote.
uint32_t Fm4ShadowSlot(const std::vector<uint32_t>& scene_slots);

}  // namespace rex::graphics
