#pragma once
// The motion a temporal upscaler gets for FM4's car ground shadow. The track draws darken the
// ground under the car from the grey map, downsampled from the car's own shadow map: an
// orthographic view from the sun centred on the car. The shadow therefore moves with the car,
// while the asphalt it falls on moves with the world. Given the asphalt's motion, a shadow
// pixel's history came from where the asphalt was a frame earlier; in reverse look that is the
// lit ground behind the shadow's edge, which DLSS kept as thin light lines across the shadow.

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace rex::graphics {

// Whether a world-view-projection (row-major for column vectors, Fm4ObjectMatrix) is
// orthographic: its w row is (0, 0, 0, 1). The car's shadow map pass draws the car so; the scene
// and the reflection cube draw it in perspective.
bool Fm4OrthographicMatrix(const std::array<float, 16>& wvp);

// The shadow map's clip space to the scene's, from the car's meshes drawn in both (each pair: a
// mesh's world-view-projection in the scene, then in the shadow map). For a rigid part both
// share its world transform, so scene * shadow^-1 is the same for all of them; the largest group
// of equal products, empty without one.
std::optional<std::array<double, 16>> Fm4ShadowToScene(
    const std::vector<std::pair<std::array<float, 16>, std::array<float, 16>>>& pairs);

// The reprojection (the current frame's clip space to the previous frame's) of what is fixed in
// the shadow map, such as the shadow on the ground, from both frames' Fm4ShadowToScene; empty
// when the current one cannot be inverted.
std::optional<std::array<double, 16>> Fm4ShadowReprojection(
    const std::array<double, 16>& previous_shadow_to_scene,
    const std::array<double, 16>& current_shadow_to_scene);

}  // namespace rex::graphics
