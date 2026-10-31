# NVIDIA DLSS Neural Rendering

ReXGlue can run NVIDIA's Neural Rendering model before a temporal upscaler.
It is optional, disabled by default, and available only in 64-bit Windows
Vulkan builds configured with the pinned NVIDIA DLSS SDK.

The SDK builds a small bridge, `bbport_nr_nvngx.dll`, adapted from MIT-licensed
source. The DLL also links NVIDIA's separately licensed parameter allocator, so
the SDK install carries the MIT and NVIDIA RTX SDK licence texts together.
NVIDIA's Neural Rendering model, `nvngx_dlssnr.dll`, is not part of this
repository and is never downloaded or installed by ReXGlue. A user who has
obtained a compatible runtime can either put it in `upscalers/dlss-nr` beside
the game or name its directory with `FM4_NR_RUNTIME` in the host application's
environment.

The host controls the bridge with these CVars:

| CVar | Values | Meaning |
|---|---|---|
| `present_nr_enabled` | `true` / `false` | Enable the model. Requires a restart so the Vulkan device is created with NGX's extensions. |
| `present_nr_preset` | `0`..`2` | Automatic, Original, or Optimized FP8 model. Optimized requires a compatible runtime. |
| `present_nr_style` | `0`..`2` | Neural Rendering style. |
| `present_nr_intensity` | `0.0`..`2.0` | Effect intensity. |
| `present_nr_structure` | `0.0`..`2.0` | Local structure strength. |
| `present_nr_tone` | `0.0`..`2.0` | Local tone strength. |

`GetVulkanNeuralRenderingStatus()` returns the model's current state for a
host UI. `IsVulkanNeuralRenderingInstalled()` checks for the bridge and the
user-provided model without initializing either.

The model receives scene color as `RGBA16_SFLOAT`, motion as `RG16_SFLOAT` in
pixels, and depth as `R32_SFLOAT` at render resolution. ReXGlue restores the
original color alpha and returns the result to the selected temporal upscaler.
If the model, driver, or input format is unavailable, the original frame and
the normal upscaler continue unchanged.

CMake controls:

- `REXGLUE_NR_BRIDGE` builds the bridge after the pinned NVIDIA DLSS SDK files
  are available. It defaults to `ON` and is ignored outside 64-bit Windows
  Vulkan builds.
- `REXGLUE_NR_RUNTIME_DLL` optionally names a user-owned
  `nvngx_dlssnr.dll` to stage with the bridge for a local build. It is never
  included in the SDK package.

The bridge ABI and provenance are documented in
[`src/ui/vulkan/nr_bridge/PROVENANCE.md`](../src/ui/vulkan/nr_bridge/PROVENANCE.md).
