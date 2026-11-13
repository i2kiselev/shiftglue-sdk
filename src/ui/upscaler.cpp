#include <rex/ui/upscaler.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace rex {
namespace ui {

float GetUpscalerModeRatio(UpscalerMode mode) {
  switch (mode) {
    case UpscalerMode::kQuality:
      return 1.5f;
    case UpscalerMode::kBalanced:
      return 1.7f;
    case UpscalerMode::kPerformance:
      return 2.0f;
    case UpscalerMode::kUltraPerformance:
      return 3.0f;
    case UpscalerMode::kAuto:
    case UpscalerMode::kNativeAa:
    default:
      return 1.0f;
  }
}

const char* GetUpscalerModeName(UpscalerMode mode) {
  switch (mode) {
    case UpscalerMode::kAuto:
      return "auto";
    case UpscalerMode::kNativeAa:
      return "nativeaa";
    case UpscalerMode::kQuality:
      return "quality";
    case UpscalerMode::kBalanced:
      return "balanced";
    case UpscalerMode::kPerformance:
      return "performance";
    case UpscalerMode::kUltraPerformance:
      return "ultra_performance";
    default:
      return "";
  }
}

namespace {
std::atomic<bool> temporal_upscaler_active = false;
std::atomic<uint32_t> presentation_height = 0;
std::atomic<float> temporal_upscaler_mip_bias = 0.0f;
}  // namespace

bool IsTemporalUpscalerActive() {
  return temporal_upscaler_active.load(std::memory_order_relaxed);
}

uint32_t GetPresentationHeight() {
  return presentation_height.load(std::memory_order_relaxed);
}

void SetPresentationHeight(uint32_t height) {
  presentation_height.store(height, std::memory_order_relaxed);
}

void SetTemporalUpscalerActive(bool active) {
  temporal_upscaler_active.store(active, std::memory_order_relaxed);
}

float TemporalUpscalerMipBias(uint32_t render_width, uint32_t output_width, float offset) {
  return std::min(0.0f, std::log2(float(render_width) / float(output_width))) + offset;
}

float GetTemporalUpscalerMipBias() {
  return temporal_upscaler_mip_bias.load(std::memory_order_relaxed);
}

bool FfxContextServesOutput(uint32_t fsr_major, uint32_t context_width, uint32_t context_height,
                            uint32_t output_width, uint32_t output_height) {
  if (fsr_major == 2) {
    return output_width == context_width && output_height == context_height;
  }
  return output_width <= context_width && output_height <= context_height;
}

bool TemporalUpscalerCutOutWithoutMipBias(bool alpha_to_mask, bool option, float bias) {
  return alpha_to_mask && option && bias != 0.0f;
}

void SetTemporalUpscalerMipBias(float bias) {
  temporal_upscaler_mip_bias.store(bias, std::memory_order_relaxed);
}

std::vector<UpscalerInfo> GetSpatialUpscalerInfos() {
  std::vector<UpscalerInfo> infos(3);
  infos[0].id = "bilinear";
  infos[0].name = "Bilinear";
  infos[1].id = "cas";
  infos[1].name = "AMD FidelityFX CAS";
  infos[1].version = "1.0";
  infos[2].id = "fsr";
  infos[2].name = "AMD FidelityFX Super Resolution 1";
  infos[2].version = "1.0";
  for (UpscalerInfo& info : infos) {
    info.kind = UpscalerKind::kSpatial;
    info.available = true;
  }
  return infos;
}

}  // namespace ui
}  // namespace rex
