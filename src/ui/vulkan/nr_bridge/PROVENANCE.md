# Neural Rendering bridge provenance

The bridge implementation, public ABI, smoke program, and license are adapted
from the MIT-licensed bbport bridge:

- Repository: `bbhostplus`
- Commit: `562b8c4819e61417439c8adb116c2b7c4699e0dc`
- Upstream directory: `gpu/nr_bridge`
- License: `LICENSE.txt` in this directory

The bridge source does not include NVIDIA code or models. The built DLL links
NVIDIA's separately licensed parameter allocator from the pinned DLSS SDK, so
the SDK installation carries the NVIDIA RTX SDK licence beside this directory's
MIT licence. The Neural Rendering model (`nvngx_dlssnr.dll`) must be supplied
by the user and is never installed by ReXGlue.

The standalone smoke program is optional and only for development:

```powershell
cmake -S src/ui/vulkan/nr_bridge -B out/nr-bridge -G "Visual Studio 17 2022" -A x64 `
  "-DDLSS_SDK_ROOT=<DLSS-SDK-root>" `
  "-DVULKAN_INCLUDE=<Vulkan-Headers>/include" `
  -DBBPORT_NR_BUILD_SMOKE=ON
cmake --build out/nr-bridge --config Release
```

In an SDK build, the bridge is controlled by `REXGLUE_NR_BRIDGE`. Supplying
`-DREXGLUE_NR_RUNTIME_DLL=<path-to-nvngx_dlssnr.dll>` stages the user-provided
runtime beside the bridge under `upscalers/dlss-nr`; without it, Neural
Rendering reports that the runtime is unavailable and leaves normal rendering
unchanged.
