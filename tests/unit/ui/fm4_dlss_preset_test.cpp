/**
 * @file        tests/unit/ui/fm4_dlss_preset_test.cpp
 *
 * @copyright   Copyright (c) 2026 i2kiselev
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <catch2/catch_test_macros.hpp>

#include <rex/ui/fm4_dlss_preset.h>

using namespace rex::ui;

namespace {

// PCI device ids as the driver reports them.
constexpr uint32_t kRtx2080Ti = 0x1E07;
constexpr uint32_t kRtx3060 = 0x2503;
constexpr uint32_t kRtx3080Ti = 0x2208;
constexpr uint32_t kRtx4070Ti = 0x2782;
constexpr uint32_t kRtx4090 = 0x2684;
constexpr uint32_t kRtx5090 = 0x2B85;

}  // namespace

TEST_CASE("RTX 40 and later take M from Performance on, K below it", "[dlss_preset]") {
  for (const uint32_t device : {kRtx4070Ti, kRtx4090, kRtx5090}) {
    // 1x to 1440p and 1x to 4K.
    CHECK(Fm4DlssAutoPreset(device, 2.0f) == Fm4DlssPreset::kM);
    CHECK(Fm4DlssAutoPreset(device, 3.0f) == Fm4DlssPreset::kM);
    // 2x to 1440p (DLAA), 1x to 1080p and 2x to 4K (Quality).
    CHECK(Fm4DlssAutoPreset(device, 1.0f) == Fm4DlssPreset::kK);
    CHECK(Fm4DlssAutoPreset(device, 1.5f) == Fm4DlssPreset::kK);
  }
}

TEST_CASE("RTX 20 and 30 take K at every ratio, M costing them twice as much", "[dlss_preset]") {
  for (const uint32_t device : {kRtx2080Ti, kRtx3060, kRtx3080Ti}) {
    for (const float ratio : {1.0f, 1.5f, 2.0f, 3.0f}) {
      CHECK(Fm4DlssAutoPreset(device, ratio) == Fm4DlssPreset::kK);
    }
  }
}
