/**
 * @file        rex/graphics/fm4_lighting.h
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
// FM4's LIGHTING settings: bloom, exposure and saturation, applied by changing
// the float constants of the game's bloom bright pass and tonemap draws (one each a frame,
// the same shaders on every track and camera; fsr4-patch/hdr/POST_CHAIN.md). The game's
// values come from each track's TrackSettings.xml, so every setting scales or offsets them.

#include <cstdint>
#include <utility>
#include <vector>

namespace rex::graphics {

// The constant register writes (register index, value) that apply the settings to a draw
// with these shaders, given the register file's values; none for any other draw.
void Fm4LightingPatches(uint64_t vertex_shader_hash, uint64_t pixel_shader_hash,
                        const uint32_t* registers,
                        std::vector<std::pair<uint32_t, uint32_t>>& patches);

}  // namespace rex::graphics
