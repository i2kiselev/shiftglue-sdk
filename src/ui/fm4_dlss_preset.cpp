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
