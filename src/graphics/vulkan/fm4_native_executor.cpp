// FM4's additions to the FH1 native executor (Vulkan), kept out of
// fh1_native_executor.cpp so that upstream's changes there merge cleanly.
// IsOneOffResolve and FlushResolveReadbacks replace upstream's definitions:
// port upstream changes to those two here, and to the readback rule itself
// in fm4_resolve_readbacks.cpp.
#include <rex/graphics/vulkan/fh1_native_executor.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/vulkan/texture_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(fh1_color_overwrite_claims, true, "GPU",
                    "Claim the tiles a rectangle overwrites in color, with or without depth (the "
                    "title's clears), without transferring their previous contents (Vulkan)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_scrambled_stencil_claims, true, "GPU",
                    "Let a depth overwrite that keeps stencil (the title's shadow map clear) "
                    "claim tiles whose stencil came from another sample layout, zeroing it "
                    "instead of transferring it (Vulkan)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_slow_frame_log, false, "GPU",
                    "Log the frames that take well above the recent median (Vulkan), with the "
                    "frame before: the GPU thread's fence waits and full queue drains, memexport "
                    "readbacks, draws, resolves and the executor's counts; with "
                    "fh1_native_gpu_profile also the slow GPU frames by phase")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_async_readbacks, false, "GPU",
                    "Steadier frame times when the GPU is fully loaded, at the risk of texture "
                    "and lighting corruption (Vulkan). Copies the resolves the CPU reads into "
                    "guest memory once their submission has completed instead of waiting for "
                    "the whole GPU queue at the guest's sync point, so the guest can see its "
                    "fence before the data, which arrives a few frames later. The first run of "
                    "a range that is new still waits")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_async_readbacks_hold, false, "GPU",
                    "Read live: while set, readbacks wait as without fh1_async_readbacks. A game "
                    "sets it where it caches what it computes from a capture read too early "
                    "(FM4: the menu car's lighting)");
REXCVAR_DEFINE_BOOL(fh1_async_recurring_readbacks, true, "GPU",
                    "Read live: with fh1_async_readbacks, the first run of a capture the game "
                    "keeps re-rendering does not wait either when it is new again only after "
                    "an idle gap (FM4: the cars' environment captures mid-race), since guest "
                    "memory still holds its previous readback; false waits as before");
REXCVAR_DEFINE_BOOL(fh1_readback_all_resolves, false, "GPU",
                    "Diagnostics: copy every resolve into guest memory, waiting for the GPU, "
                    "not only the captures the CPU is known to read (Vulkan; slow)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fh1_readback_all_resolves_hold, false, "GPU",
                    "Read live: while set, every resolve is read back and waited for, as with "
                    "fh1_readback_all_resolves. A game sets it around a render whose result the "
                    "CPU reads in ways the readback rules miss (FM4: the car thumbnails, lit "
                    "from captures re-rendered past a run's first frames or after a range "
                    "stopped being new)");
REXCVAR_DEFINE_BOOL(fh1_log_resolve_readbacks, false, "GPU",
                    "Diagnostics: log the start of every run of resolves into a range, whether "
                    "it is read back into guest memory and whether the range was read back before");
REXCVAR_DEFINE_BOOL(fh1_surfaceless_clears, true, "GPU",
                    "Keep the whole tiles a clear sets to one value as that value instead of in a "
                    "surface of the clear's layout (D3D's fast clears through a 4x MSAA view); "
                    "the surface claiming them next clears them (Vulkan)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DECLARE(bool, fh1_scaled_msaa_single_sample);
REXCVAR_DECLARE(std::string, fh1_resolve_dump_dir);
REXCVAR_DECLARE(int32_t, fh1_resolve_dump_frame);

namespace rex::graphics::vulkan {

namespace {

// Xenos 7e3 from a float as the render backend packs it
// (spirv_translator_rb.cpp): clamped to [0, 31.875], rounded to nearest even.
uint32_t Float32To7e3(float value) {
  if (!(value > 0.0f)) value = 0.0f;
  value = std::min(value, 31.875f);
  uint32_t f32;
  std::memcpy(&f32, &value, sizeof(f32));
  const uint32_t biased = f32 < 0x3E800000
                              ? ((f32 & 0x7FFFFF) | 0x800000) >> std::min(125 - (f32 >> 23), 24u)
                              : f32 - (UINT32_C(124) << 23);
  return ((biased + 0x7FFF + ((biased >> 16) & 1)) >> 16) & 0x3FF;
}

// The EDRAM word a 32bpp color target holds for a shader output, as the host
// render targets store it: scaled by the target's exponent bias, 16_16 as
// snorm over -1...1 (or -32...32 unless `fixed16_truncated`). False for
// formats not packed here.
bool PackColorWord(reg::RB_COLOR_INFO color_info, bool fixed16_truncated,
                   std::array<float, 4> color, uint32_t& word_out) {
  const float bias_scale = std::ldexp(1.0f, int32_t(color_info.color_exp_bias));
  for (float& channel : color) channel *= bias_scale;
  auto unorm = [](float value, float max) {
    return uint32_t((value > 0.0f ? std::min(value, 1.0f) : 0.0f) * max + 0.5f);
  };
  switch (color_info.color_format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      word_out = 0;
      for (uint32_t i = 0; i < 4; ++i) word_out |= unorm(color[i], 255.0f) << (8 * i);
      return true;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      word_out = unorm(color[3], 3.0f) << 30;
      for (uint32_t i = 0; i < 3; ++i) word_out |= unorm(color[i], 1023.0f) << (10 * i);
      return true;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      word_out = unorm(color[3], 3.0f) << 30;
      for (uint32_t i = 0; i < 3; ++i) word_out |= Float32To7e3(color[i]) << (10 * i);
      return true;
    case xenos::ColorRenderTargetFormat::k_16_16:
      word_out = 0;
      for (uint32_t i = 0; i < 2; ++i) {
        const float value = std::isnan(color[i]) ? 0.0f
                                                 : color[i] * (fixed16_truncated ? 1.0f : 1.0f / 32.0f);
        const float scaled = std::clamp(value, -1.0f, 1.0f) * 32767.0f;
        word_out |= (uint32_t(int32_t(scaled + (scaled < 0.0f ? -0.5f : 0.5f))) & 0xFFFF)
                    << (16 * i);
      }
      return true;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      std::memcpy(&word_out, &color[0], sizeof(word_out));
      return true;
    default:
      return false;
  }
}

}  // namespace

void Fh1NativeExecutor::LoadFm4Options() {
  color_overwrite_claims_ = REXCVAR_GET(fh1_color_overwrite_claims);
  scrambled_stencil_claims_ = REXCVAR_GET(fh1_scrambled_stencil_claims);
  surfaceless_clears_ = REXCVAR_GET(fh1_surfaceless_clears);
  slow_frame_log_ = REXCVAR_GET(fh1_slow_frame_log);
  async_readbacks_ = REXCVAR_GET(fh1_async_readbacks);
  clear_words_.clear();
}

bool Fh1NativeExecutor::IsSingleSampleMsaaOutdated() const {
  return single_sample_msaa_ != (scale_ >= 2 && REXCVAR_GET(fh1_scaled_msaa_single_sample));
}

void Fh1NativeExecutor::ReleaseIdleSurfaces() {
  constexpr uint64_t kIdleFrames = 600;
  bool released = false;
  for (auto it = surfaces_.begin(); it != surfaces_.end();) {
    // A surface that owns tiles holds their only copy.
    Surface& surface = it->second;
    if (frame_ < surface.last_used_frame + kIdleFrames || tiles_.OwnsAny(it->first)) {
      ++it;
      continue;
    }
    RetiredImage retired;
    retired.submission = command_processor_.GetCurrentSubmission();
    retired.image = surface.image;
    retired.memory = surface.memory;
    retired.views[0] = surface.view;
    retired.views[1] = surface.uint_view;
    retired.views[2] =
        surface.sampled_view != surface.view && surface.sampled_view != surface.uint_view
            ? surface.sampled_view
            : VK_NULL_HANDLE;
    retired.views[3] = surface.stencil_view;
    retired_images_.push_back(retired);
    if (!surface.pending_clears.empty()) --pending_clear_surfaces_;
    surface_bytes_ -= surface.bytes;
    it = surfaces_.erase(it);
    released = true;
    Count("surface_released");
  }
  if (!released) return;
  // Preparations that found their surfaces in place must look again.
  for (SurfaceFront& front : surface_front_) front = SurfaceFront();
  for (PrepareMemo& memo : prepare_memos_) memo.state_epoch = 0;
  for (PrepareSignature& recent : recent_prepares_) recent = PrepareSignature();
}

void Fh1NativeExecutor::ReleaseRetiredBuffers(bool all) {
  if (retired_buffers_.empty()) return;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const uint64_t completed = command_processor_.GetCompletedSubmission();
  std::erase_if(retired_buffers_, [&](const RetiredBuffer& retired) {
    if (!all && retired.submission > completed) return false;
    dfn.vkDestroyBuffer(device, retired.buffer, nullptr);
    dfn.vkFreeMemory(device, retired.memory, nullptr);
    return true;
  });
}

bool Fh1NativeExecutor::StencilScrambled(const SurfaceKey& key, uint32_t owner) {
  if (!scrambled_stencil_claims_ || IsClearOwner(owner)) return false;
  Surface* source = owner == kNoOwner ? nullptr : FindSurface(owner);
  // Stencil read through another pitch or sample count, or a color owner's
  // bits, lands on unrelated samples, so no draw can depend on keeping it.
  return !source || !source->key.is_depth || source->key.pitch_tiles != key.pitch_tiles ||
         source->key.msaa != key.msaa;
}

bool Fh1NativeExecutor::DrawOverwritesColor(const Fh1DrawInfo& draw, uint32_t used_bits) const {
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(used_bits & (1u << (1 + i)))) continue;
    if (((draw.normalized_color_mask >> (4 * i)) & 0xF) != 0xF) return false;
    const auto blend =
        register_file_.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[i]);
    if (blend.color_srcblend != xenos::BlendFactor::kOne ||
        blend.color_destblend != xenos::BlendFactor::kZero ||
        blend.color_comb_fcn != xenos::BlendOp::kAdd ||
        blend.alpha_srcblend != xenos::BlendFactor::kOne ||
        blend.alpha_destblend != xenos::BlendFactor::kZero ||
        blend.alpha_comb_fcn != xenos::BlendOp::kAdd) {
      return false;
    }
  }
  return true;
}

bool Fh1NativeExecutor::MayOverwrite(const Fh1DrawInfo& draw, uint32_t used_bits) const {
  const reg::RB_DEPTHCONTROL depth_control = draw.normalized_depth_control;
  if (used_bits & 1) {
    return (color_overwrite_claims_ || used_bits == 1) && depth_control.z_enable &&
           depth_control.z_write_enable &&
           depth_control.zfunc == xenos::CompareFunction::kAlways;
  }
  // Depth and stencil off: a rectangle list that replaces its color targets.
  return color_overwrite_claims_ && used_bits && !depth_control.z_enable &&
         !depth_control.stencil_enable &&
         register_file_.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
             xenos::PrimitiveType::kRectangleList &&
         DrawOverwritesColor(draw, used_bits);
}

bool Fh1NativeExecutor::ClaimColorOverwriteTiles(const SurfaceKey& key, uint32_t length) {
  if (key.Is64bpp()) return false;
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t tile_width = xenos::kEdramTileWidthSamples >> msaa_x_log2;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  for (const auto& rect : depth_overwrite_.rects()) {
    const uint32_t column_end = (uint32_t(rect.outer[2]) + tile_width - 1) / tile_width;
    const uint32_t row_end = (uint32_t(rect.outer[3]) + tile_height - 1) / tile_height;
    if (column_end > key.pitch_tiles || (row_end - 1) * key.pitch_tiles + column_end > length) {
      return false;
    }
  }
  const uint32_t packed_key = key.Pack();
  for (const auto& rect : depth_overwrite_.rects()) {
    const uint32_t column_first = uint32_t(rect.outer[0]) / tile_width;
    const uint32_t column_end = (uint32_t(rect.outer[2]) + tile_width - 1) / tile_width;
    const uint32_t row_first = uint32_t(rect.outer[1]) / tile_height;
    const uint32_t row_end = (uint32_t(rect.outer[3]) + tile_height - 1) / tile_height;
    uint32_t inner_column_first = (uint32_t(rect.inner[0]) + tile_width - 1) / tile_width;
    uint32_t inner_column_end = uint32_t(rect.inner[2]) / tile_width;
    uint32_t inner_row_first = (uint32_t(rect.inner[1]) + tile_height - 1) / tile_height;
    uint32_t inner_row_end = uint32_t(rect.inner[3]) / tile_height;
    if (inner_column_first >= inner_column_end || inner_row_first >= inner_row_end) {
      inner_column_first = inner_column_end = column_first;
      inner_row_first = inner_row_end = row_first;
    }
    // Tiles the draw covers whole take no transfer; the rest of its bounds do.
    for (uint32_t row = inner_row_first; row < inner_row_end; ++row) {
      ClaimTiles(key.base_tiles + row * key.pitch_tiles + inner_column_first,
                 inner_column_end - inner_column_first, packed_key, false);
    }
    ClaimTileRect(key, column_first, row_first, column_end, inner_row_first);
    ClaimTileRect(key, column_first, inner_row_end, column_end, row_end);
    ClaimTileRect(key, column_first, inner_row_first, inner_column_first, inner_row_end);
    ClaimTileRect(key, inner_column_end, inner_row_first, column_end, inner_row_end);
    for (uint32_t row = row_first; row < row_end; ++row) {
      MarkTileStencil(key.base_tiles + row * key.pitch_tiles + column_first,
                      column_end - column_first, true);
    }
  }
  Count("color_overwrite_draw");
  return true;
}

uint32_t Fh1NativeExecutor::ClearOwner(uint32_t word) {
  auto it = std::find(clear_words_.begin(), clear_words_.end(), word);
  if (it != clear_words_.end()) return kClearOwner | uint32_t(it - clear_words_.begin());
  if (clear_words_.size() < kClearWordsMax) {
    REXGPU_INFO("FH1 native executor (Vulkan): clear record {} holds {:08X}", clear_words_.size(),
                word);
    clear_words_.push_back(word);
    return kClearOwner | uint32_t(clear_words_.size() - 1);
  }
  std::array<bool, kClearWordsMax> held = {};
  for (uint32_t tile = 0; tile < xenos::kEdramTileCount; ++tile) {
    const uint32_t owner = tiles_.Owner(tile);
    if (IsClearOwner(owner)) held[owner & ~kClearOwner] = true;
  }
  for (uint32_t i = 0; i < kClearWordsMax; ++i) {
    if (held[i]) continue;
    clear_words_[i] = word;
    return kClearOwner | i;
  }
  return kNoOwner;
}

uint32_t Fh1NativeExecutor::DeriveClearWords(const Fh1DrawInfo& draw, uint32_t used_bits,
                                             bool stencil_overwritten, uint32_t* words_out) {
  if (!surfaceless_clears_ || draw.occlusion_query_active) return 0;
  const RegisterFile& regs = register_file_;
  uint32_t known_bits = 0;
  float depth;
  if ((used_bits & 1) && depth_overwrite_.ConstantDepth(depth)) {
    depth = std::isnan(depth) ? 0.0f : std::clamp(depth, 0.0f, 1.0f);
    const uint32_t depth_bits =
        regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8
            ? xenos::Float32To20e4(depth, true)
            : uint32_t(std::nearbyint(depth * float(0xFFFFFF)));
    // Stencil the draw keeps is zero, or AbsorbClear finds it is not.
    const uint32_t stencil =
        stencil_overwritten
            ? regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK).stencilref
            : 0;
    words_out[0] = (depth_bits << 8) | stencil;
    known_bits |= 1;
  }
  std::array<std::array<float, 4>, 4> colors;
  uint32_t colors_written = 0;
  if ((used_bits & ~1u) && draw.pixel_shader &&
      depth_overwrite_.DeriveColors(*draw.pixel_shader, colors, colors_written)) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (!(used_bits & (1u << (1 + i))) || !(colors_written & (1u << i))) continue;
      if (PackColorWord(regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]),
                        config_.fixed16_truncated, colors[i], words_out[1 + i])) {
        known_bits |= 1u << (1 + i);
      }
    }
  }
  return known_bits;
}

bool Fh1NativeExecutor::AbsorbClear(const SurfaceKey& key, uint32_t length, uint32_t word,
                                    bool stencil_kept) {
  if (key.Is64bpp()) return false;
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const int32_t tile_width = int32_t(xenos::kEdramTileWidthSamples >> msaa_x_log2);
  const int32_t tile_height = int32_t(xenos::kEdramTileHeightSamples >> msaa_y_log2);
  for (const auto& rect : depth_overwrite_.rects()) {
    if (rect.inner != rect.outer || rect.outer[0] % tile_width || rect.outer[2] % tile_width ||
        rect.outer[1] % tile_height || rect.outer[3] % tile_height) {
      return false;
    }
    const uint32_t column_end = uint32_t(rect.outer[2] / tile_width);
    const uint32_t row_end = uint32_t(rect.outer[3] / tile_height);
    if (column_end > key.pitch_tiles || (row_end - 1) * key.pitch_tiles + column_end > length) {
      return false;
    }
    if (!stencil_kept) continue;
    for (uint32_t row = uint32_t(rect.outer[1] / tile_height); row < row_end; ++row) {
      for (uint32_t column = uint32_t(rect.outer[0] / tile_width); column < column_end; ++column) {
        const uint32_t tile = key.base_tiles + row * key.pitch_tiles + column;
        if (tiles_.StencilNonzero(tile) && !StencilScrambled(key, tiles_.Owner(tile))) {
          return false;
        }
      }
    }
  }
  const uint32_t owner = ClearOwner(word);
  if (owner == kNoOwner) return false;
  for (const auto& rect : depth_overwrite_.rects()) {
    const uint32_t column_first = uint32_t(rect.outer[0] / tile_width);
    const uint32_t column_end = uint32_t(rect.outer[2] / tile_width);
    for (uint32_t row = uint32_t(rect.outer[1] / tile_height);
         row < uint32_t(rect.outer[3] / tile_height); ++row) {
      const uint32_t row_base = key.base_tiles + row * key.pitch_tiles + column_first;
      tiles_.Claim(row_base, column_end - column_first, owner, false);
      MarkTileStencil(row_base, column_end - column_first, (word & 0xFF) != 0);
    }
  }
  Count(key.is_depth ? "clear_absorbed_depth" : "clear_absorbed_color");
  return true;
}

void Fh1NativeExecutor::MaterializeClears(const SurfaceKey& key, int32_t x0, int32_t y0,
                                          int32_t x1, int32_t y1) {
  const uint32_t msaa_x_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k4X));
  const uint32_t msaa_y_log2 = uint32_t(key.msaa >= uint32_t(xenos::MsaaSamples::k2X));
  const uint32_t is_64bpp = key.Is64bpp() ? 1 : 0;
  const uint32_t tile_width = (xenos::kEdramTileWidthSamples >> msaa_x_log2) >> is_64bpp;
  const uint32_t tile_height = xenos::kEdramTileHeightSamples >> msaa_y_log2;
  const uint32_t pitch = key.pitch_tiles << is_64bpp;
  if (!GetOrCreateSurface(key, uint32_t(y1))) return Skip("clear_materialize_surface");
  const uint32_t column_first = uint32_t(x0) / tile_width;
  const uint32_t column_end = (uint32_t(x1) + tile_width - 1) / tile_width;
  for (uint32_t row = uint32_t(y0) / tile_height;
       row < (uint32_t(y1) + tile_height - 1) / tile_height; ++row) {
    for (uint32_t column = column_first; column < column_end;) {
      const uint32_t first = key.base_tiles + row * pitch + column;
      uint32_t count = 0;
      while (column < column_end && IsClearOwner(tiles_.Owner(first + count))) {
        ++count;
        ++column;
      }
      if (count) {
        ClaimTiles(first, count, key.Pack());
      } else {
        ++column;
      }
    }
  }
  FlushTransfers();
  Count("clear_materialized");
  if (command_processor_.debug_log_draws_frame()) {
    REXGPU_INFO("FH1 debug clear materialized for {}", key.Describe());
  }
}

void Fh1NativeExecutor::ClearClaimedTiles(Surface& dest, uint32_t clear_owner, const Rect* rects,
                                          uint32_t rect_count, uint32_t tile_count) {
  const uint32_t word = clear_words_[clear_owner & ~kClearOwner];
  for (uint32_t i = 0; i < rect_count; ++i) ClearSurfaceRect(dest, rects[i], word, word);
  counters_.Count("clear_claimed_tiles", tile_count);
  if (command_processor_.debug_log_draws_frame()) {
    REXGPU_INFO("FH1 debug transfer clear{:08X}->{} tiles={} passes=0", word, dest.key.Describe(),
                tile_count);
  }
}

void Fh1NativeExecutor::MaterializeResolveClears(CopyPlan& plan) {
  if (std::none_of(plan.sources.begin(), plan.sources.end(),
                   [](const SourceRect& source) { return source.cleared; })) {
    return;
  }
  MaterializeClears(plan.copying_depth
                        ? plan.resolve_key
                        : MakeColorKey(plan.color_info.color_base, plan.pitch_tiles, plan.msaa,
                                       plan.color_info.color_format),
                    plan.x0, plan.y0, plan.x1, plan.y1);
  GetResolveSources(plan.resolve_key, plan.x0, plan.y0, plan.x1, plan.y1, plan.sources);
}

void Fh1NativeExecutor::DumpResolveOnRequest(uint32_t address, uint32_t length) {
  if (REXCVAR_GET(fh1_resolve_dump_dir).empty()) return;
  const int32_t dump_frame = REXCVAR_GET(fh1_resolve_dump_frame);
  if (dump_frame < 0 && dump_trigger_checked_frame_ != frame_) {
    // A negative frame dumps the frame after a file named "trigger" appears
    // in the directory, so a running game can be asked for one frame.
    dump_trigger_checked_frame_ = frame_;
    const auto trigger = std::filesystem::path(REXCVAR_GET(fh1_resolve_dump_dir)) / "trigger";
    std::error_code error;
    if (std::filesystem::remove(trigger, error)) {
      dump_trigger_frame_ = frame_;
      command_processor_.SetDebugLogDrawsFrame(true);
    }
    DumpScaledResolveRanges();
  }
  if (!dump_frame || frame_ == uint64_t(dump_frame) ||
      (dump_frame < 0 && frame_ == dump_trigger_frame_)) {
    DumpResolveOutput(address, length);
  }
}

void Fh1NativeExecutor::DumpScaledResolveRanges() {
  // "dump_scaled" in fh1_resolve_dump_dir lists "<address> <length>" in hex per
  // line, as the command processor's "dump_memory": each range's part of the
  // scaled resolve buffer, which scaled textures there sample and a texture
  // flush reloads unchanged, goes to scaled_<address>_<length>.bin.
  const std::filesystem::path dump_dir(REXCVAR_GET(fh1_resolve_dump_dir));
  std::ifstream list(dump_dir / "dump_scaled");
  if (!list) return;
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  uint32_t address, length;
  list >> std::hex;
  while (list >> address >> length) ranges.emplace_back(address & 0x1FFFFFFF, length);
  list.close();
  std::error_code error;
  std::filesystem::remove(dump_dir / "dump_scaled", error);
  if (scale_ <= 1) return;
  for (const auto& [range_address, range_length] : ranges) {
    uint64_t start, size;
    // Committing first: a copy from unbound sparse memory is undefined.
    if (!config_.textures->GetScaledResolveRange(range_address, range_length, 0, start, size) ||
        !config_.textures->CommitScaledResolveRange(range_address, range_length)) {
      continue;
    }
    command_processor_.PushBufferMemoryBarrier(
        config_.textures->scaled_resolve_buffer(), start, size,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, false);
    DumpBufferRange(config_.textures->scaled_resolve_buffer(), start, size,
                    fmt::format("scaled_{:08X}_{:X}.bin", range_address, range_length));
    REXGPU_INFO("FH1 debug scaled {:08X}+{:X}: scale {} bytes {:X} at {:X}", range_address,
                range_length, scale_, size, start);
  }
}

void Fh1NativeExecutor::DumpBufferRange(VkBuffer source, VkDeviceSize offset, VkDeviceSize length,
                                        const std::string& name) {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkBuffer buffer;
  VkDeviceMemory memory;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, length, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, buffer, memory)) {
    return;
  }
  command_processor_.SubmitBarriers(true);
  const VkBufferCopy region = {offset, 0, length};
  command_processor_.deferred_command_buffer().CmdVkCopyBuffer(source, buffer, 1, &region);
  command_processor_.Fh1AwaitAllQueueOperations();
  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
    VkMappedMemoryRange range = {};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = memory;
    range.size = VK_WHOLE_SIZE;
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
    std::ofstream(std::filesystem::path(REXCVAR_GET(fh1_resolve_dump_dir)) / name,
                  std::ios::binary)
        .write(static_cast<const char*>(mapped), std::streamsize(length));
    dfn.vkUnmapMemory(device, memory);
  }
  dfn.vkDestroyBuffer(device, buffer, nullptr);
  dfn.vkFreeMemory(device, memory, nullptr);
}

bool Fh1NativeExecutor::IsOneOffResolve(uint32_t address, uint32_t length) {
  const auto decision =
      resolve_readbacks_.Resolve(address, length, frame_,
                                 REXCVAR_GET(fh1_readback_all_resolves) ||
                                     REXCVAR_GET(fh1_readback_all_resolves_hold));
  if (REXCVAR_GET(fh1_log_resolve_readbacks) && decision.log) {
    REXGPU_INFO("FH1 readback run {:08X}+{:X} frame {} {}: read back {}, before {}, "
                "overwritten {}, recurring {}, idle {}, new since {}, previous run {} frames",
                address, length, frame_, decision.run_start ? "start" : "continues",
                decision.read_back, decision.read_back_before, decision.overwritten,
                decision.recurring,
                decision.idle_frames, decision.new_since_frames, decision.previous_run_frames);
  }
  return decision.read_back;
}

void Fh1NativeExecutor::FlushResolveReadbacks() {
  ApplyCompletedDeferredReadbacks();
  if (pending_readbacks_.empty()) return;
  if (!Fm4ReadbacksWaitForGpu(
          async_readbacks_, REXCVAR_GET(fh1_async_readbacks_hold),
          std::any_of(pending_readbacks_.begin(), pending_readbacks_.end(),
                      [](const PendingReadback& pending) { return pending.wait; }))) {
    CompleteResolveReadbacks(command_processor_.GetCompletedSubmission());
    return;
  }
  command_processor_.Fh1AwaitAllQueueOperations();
  CompleteResolveReadbacks(UINT64_MAX);
  // Everything has completed now, the deferred copies included.
  ApplyCompletedDeferredReadbacks();
  Count("resolve_readback_flush");
}

void Fh1NativeExecutor::CompleteResolveReadbacks(uint64_t completed_submission) {
  if (pending_readbacks_.empty() ||
      pending_readbacks_.front().submission > completed_submission) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkMappedMemoryRange range = {};
  range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  range.memory = readback_memory_;
  range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
  // In order: a later resolve over the same bytes wins, as on the GPU.
  size_t completed = 0;
  for (; completed < pending_readbacks_.size(); ++completed) {
    const PendingReadback& pending = pending_readbacks_[completed];
    if (pending.submission > completed_submission) break;
    config_.memory->CopyReadbackToGuest(pending.address, readback_mapped_ + pending.offset,
                                        pending.length);
  }
  pending_readbacks_.erase(pending_readbacks_.begin(), pending_readbacks_.begin() + completed);
  // Only an empty buffer is filled from its start again.
  if (pending_readbacks_.empty()) readback_used_ = 0;
}

void Fh1NativeExecutor::LogProfiledResolve(const CopyPlan& plan) {
  if (!REXCVAR_GET(fh1_native_gpu_profile)) return;
  const auto& dest = plan.info.copy_dest_info;
  const std::string detail = fmt::format(
      "{} {}x{} number={} exp_bias={} endian={} swap={} pitch={} at {:08X}", plan.kind,
      plan.x1 - plan.x0, plan.y1 - plan.y0, uint32_t(dest.copy_dest_number),
      int32_t(dest.copy_dest_exp_bias), uint32_t(dest.copy_dest_endian),
      uint32_t(dest.copy_dest_swap), plan.dest_pitch, plan.info.copy_dest_extent_start);
  if (reported_resolve_skips_.emplace(detail, "").second) {
    REXGPU_INFO("FH1 native executor (Vulkan): resolve {}", detail);
  }
}

void Fh1NativeExecutor::LogSurfaces() {
  if (gpu_query_pool_ == VK_NULL_HANDLE) return;
  std::string list;
  for (const auto& [packed, surface] : surfaces_) {
    list += fmt::format(" {}:{}/{}rows:{}mb", surface.key.Describe(), surface.image_height,
                        SurfaceHeight(surface.key.pitch_tiles, surface.key.msaa),
                        surface.bytes >> 20);
  }
  REXGPU_INFO("FH1 native executor (Vulkan) surfaces:{}", list);
}

float Fh1NativeExecutor::RecentMedian::Median() const {
  if (count_ < 60) return 0.0f;
  std::array<float, 240> sorted = values_;
  auto middle = sorted.begin() + count_ / 2;
  std::nth_element(sorted.begin(), middle, sorted.begin() + count_);
  return *middle;
}

void Fh1NativeExecutor::RecentMedian::Add(float value) {
  values_[next_] = value;
  next_ = (next_ + 1) % values_.size();
  count_ = std::min(count_ + 1, values_.size());
}

void Fh1NativeExecutor::LogSlowFrame() {
  const auto now = std::chrono::steady_clock::now();
  const float ms = slow_frame_last_swap_.time_since_epoch().count()
                       ? std::chrono::duration<float, std::milli>(now - slow_frame_last_swap_).count()
                       : 0.0f;
  slow_frame_last_swap_ = now;
  const VulkanCommandProcessor::Fh1FrameWaits waits = command_processor_.TakeFh1FrameWaits();
  std::string stats;
  for (const auto& [name, count] : counters_.stats()) {
    uint64_t& seen = slow_frame_stats_[name];
    if (count != seen) {
      stats += fmt::format(" {}={}", name, count - seen);
      seen = count;
    }
  }
  const std::string summary = fmt::format(
      "draws {} resolves {} fence_wait {:.1f} ms drains {} ({:.1f} ms) memexport {} (full {}){}",
      draws_ - slow_frame_draws_, resolves_ - slow_frame_resolves_, waits.fence_wait_ns / 1e6,
      waits.drains, waits.drain_wait_ns / 1e6, waits.memexport_readbacks,
      waits.memexport_full_readbacks, stats);
  slow_frame_draws_ = draws_;
  slow_frame_resolves_ = resolves_;
  if (ms > 0.0f) {
    const float median = slow_frame_intervals_.Median();
    if (IsSlow(ms, median)) {
      REXGPU_INFO("FH1 slow frame {}: {:.1f} ms (median {:.1f}): {} | before: {}", frame_, ms,
                  median, summary, slow_frame_previous_);
    }
    slow_frame_intervals_.Add(ms);
  }
  slow_frame_previous_ = summary;
}

void Fh1NativeExecutor::LogSlowGpuFrame(uint64_t frame,
                                        const std::array<uint64_t, kGpuPhases>& ticks) {
  const double tick_ms = command_processor_.GetVulkanDevice()->properties().timestampPeriod / 1e6;
  const float ms = float(ticks[kGpuFrame] * tick_ms);
  const float median = slow_gpu_frames_.Median();
  if (IsSlow(ms, median)) {
    REXGPU_INFO(
        "FH1 slow GPU frame after {}: {:.1f} ms (median {:.1f}): transfers {:.2f} "
        "resolves {:.2f} clears {:.2f} texture_reloads {:.2f} texture_loads {:.2f}",
        frame, ms, median, ticks[kGpuTransfers] * tick_ms, ticks[kGpuResolves] * tick_ms,
        ticks[kGpuClears] * tick_ms, ticks[kGpuTextureReloads] * tick_ms,
        ticks[kGpuTextureLoads] * tick_ms);
  }
  slow_gpu_frames_.Add(ms);
}

}  // namespace rex::graphics::vulkan
