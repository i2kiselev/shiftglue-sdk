/**
 * @file        rex/ui/fm4_dlss_preset.h
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
// The DLSS preset present_dlss_preset=auto picks. NVIDIA's programming guide (310.6.0 table,
// Performance mode to 1440p) has M at 1.2-1.3x K's cost on RTX 40 and later but about 2x
// on RTX 20 and 30 (3060: K 2.39 ms, M 5.05 ms), where M is not "peak performant".

#include <cstdint>

namespace rex::ui {

enum class Fm4DlssPreset { kK, kM };

// device_id is an NVIDIA PCI device id; ratio is the output width over the render width.
Fm4DlssPreset Fm4DlssAutoPreset(uint32_t device_id, float ratio);

}  // namespace rex::ui
