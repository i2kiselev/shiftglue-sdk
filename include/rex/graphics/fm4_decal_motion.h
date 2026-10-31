/**
 * @file        rex/graphics/fm4_decal_motion.h
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
// The motion a temporal upscaler gets for FM4's other cars' ground shadows. Each car's shadow
// is drawn after the scene as its parts flattened onto the ground (the body's mesh, a pair of
// meshes under each wheel), multiplying the asphalt's colour without writing depth; their
// matrices are singular, and the pixels kept the asphalt's motion while the shadow moved with
// the car: a trail behind cars under DLAA and fizzle at the tyres, where the shadow is darkest.
// A shadow draw reads the same world matrix (c8-c11) as the part it belongs to, drawn earlier
// with an object slot; its darkened pixels take that slot (a third plane of the object slot
// image).

#include <array>
#include <cstdint>

namespace rex::graphics {

// Whether a draw is such a shadow, from its RB_DEPTHCONTROL, RB_BLENDCONTROL0 and RB_COLOR_MASK:
// onto the scene's colour surface, multiplying it (source times destination colour) and not
// writing depth. Car glass's tint is one too, and moves with the car as well.
bool Fm4ShadowDecalDraw(uint32_t depth_control, uint32_t blend_control, uint32_t color_mask,
                        bool writes_scene_color);

// Whether a vertex shader (its float constant bitmap) reads a part's world matrix: c8-c11 in
// FM4's car shaders.
bool Fm4ReadsWorldMatrix(const std::array<uint64_t, 4>& float_bitmap);

}  // namespace rex::graphics
