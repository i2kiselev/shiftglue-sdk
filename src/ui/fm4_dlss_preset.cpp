/**
 * @file        ui/fm4_dlss_preset.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/fm4_dlss_preset.h>

namespace rex::ui {

Fm4DlssPreset Fm4DlssAutoPreset(uint32_t device_id, float ratio) {
  // Turing and Ampere end below 0x2600; Ada starts at AD102's 0x2684.
  const bool ada_or_newer = device_id >= 0x2600;
  // DLSS's Performance mode and beyond (PerfQualityForRatio).
  const bool performance = ratio > 1.8f;
  return ada_or_newer && performance ? Fm4DlssPreset::kM : Fm4DlssPreset::kK;
}

}  // namespace rex::ui
