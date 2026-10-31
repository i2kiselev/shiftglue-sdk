/**
 ******************************************************************************
 * ReXGlue native Vulkan integration for DLSS Neural Rendering.
 *
 * This file contains the ShiftGlue adapter only. The feature model remains in
 * NVIDIA's runtime and the small ABI bridge beside it is independently MIT
 * licensed under nr_bridge/LICENSE.txt.
 ******************************************************************************
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/util.h>
#include <rex/ui/vulkan/vulkan_neural_rendering.h>

namespace {
namespace shaders {
#include "../shaders/vulkan_spirv/neural_rendering_color_cs.h"
#include "../shaders/vulkan_spirv/neural_rendering_motion_cs.h"
#include "../shaders/vulkan_spirv/neural_rendering_pack_cs.h"
}  // namespace shaders
}  // namespace

REXCVAR_DEFINE_BOOL(present_nr_enabled, false, "UI/Vulkan",
                    "Enable NVIDIA DLSS Neural Rendering before the selected temporal "
                    "upscaler; requires an RTX GPU and the NR runtime beside the game")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(present_nr_preset, 0, "UI/Vulkan",
                     "Neural Rendering model: 0 automatic, 1 original, 2 optimized FP8")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(present_nr_style, 0, "UI/Vulkan", "Neural Rendering style: 0, 1 or 2")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(present_nr_intensity, 1.0, "UI/Vulkan", "Neural Rendering intensity")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(present_nr_structure, 1.0, "UI/Vulkan",
                      "Neural Rendering local structure strength")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(present_nr_tone, 1.0, "UI/Vulkan", "Neural Rendering local tone strength")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex {
namespace ui {
namespace vulkan {

namespace {

std::mutex g_status_mutex;
std::string g_status = "Disabled";

void SetStatus(const char* status) {
  const std::string next = status ? status : "";
  bool changed = false;
  {
    const std::lock_guard lock(g_status_mutex);
    changed = g_status != next;
    if (changed) {
      g_status = next;
    }
  }
  if (changed) {
    REXLOG_INFO("Neural Rendering: {}", next);
  }
}

#if REX_PLATFORM_WIN32

#include "nr_bridge/bbport_nr_bridge.h"

constexpr uint32_t kNvidiaVendorId = 0x10DE;
constexpr uint32_t kDescriptorSetCount = 9;
constexpr uint32_t kDispatchGroupSize = 8;

struct BridgeRuntime {
  HMODULE module = nullptr;
  const BbNrApi* api = nullptr;
  std::filesystem::path runtime_directory;
  std::filesystem::path data_directory;
  bool attempted = false;
  bool configured = false;

  const wchar_t* runtime_path() const { return runtime_directory.c_str(); }
  const wchar_t* data_path() const { return data_directory.c_str(); }
};

BridgeRuntime& Runtime() {
  static BridgeRuntime runtime;
  return runtime;
}

std::filesystem::path ModuleDirectory() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module)) {
    return {};
  }
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
  if (!length || length >= path.size()) {
    return {};
  }
  path.resize(length);
  return std::filesystem::path(path).parent_path();
}

std::filesystem::path FindRuntimeDirectory(const std::filesystem::path& executable_directory) {
  std::vector<std::filesystem::path> candidates;
  if (const wchar_t* env = _wgetenv(L"FM4_NR_RUNTIME"); env && *env) {
    candidates.emplace_back(env);
  }
  candidates.push_back(executable_directory);
  candidates.push_back(executable_directory / L"upscalers" / L"dlss-nr");
  candidates.push_back(executable_directory / L"upscalers" / L"dlssnr");
  for (const std::filesystem::path& candidate : candidates) {
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate / L"nvngx_dlssnr.dll", error)) {
      return candidate;
    }
  }
  return {};
}

std::filesystem::path FindBridge(const std::filesystem::path& executable_directory,
                                 const std::filesystem::path& runtime_directory) {
  for (const std::filesystem::path& directory :
       {runtime_directory, executable_directory, executable_directory / L"upscalers" / L"dlss-nr",
        executable_directory / L"upscalers" / L"dlssnr"}) {
    if (directory.empty()) {
      continue;
    }
    const std::filesystem::path candidate = directory / L"bbport_nr_nvngx.dll";
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error)) {
      return candidate;
    }
  }
  return {};
}

void LogBridge(int warning, const char* message) {
  if (warning) {
    REXLOG_WARN("{}", message ? message : "Neural Rendering warning");
  } else {
    REXLOG_INFO("{}", message ? message : "");
  }
}

bool PrepareRuntime() {
  BridgeRuntime& runtime = Runtime();
  if (runtime.attempted) {
    return runtime.configured;
  }
  runtime.attempted = true;
  if (!REXCVAR_GET(present_nr_enabled)) {
    return false;
  }

  const std::filesystem::path executable_directory = ModuleDirectory();
  runtime.runtime_directory = FindRuntimeDirectory(executable_directory);
  if (runtime.runtime_directory.empty()) {
    SetStatus("Unavailable: NR runtime not installed");
    REXLOG_WARN(
        "Neural Rendering: nvngx_dlssnr.dll is not installed beside the game "
        "(FM4_NR_RUNTIME can select its folder)");
    return false;
  }

  const std::filesystem::path bridge_path =
      FindBridge(executable_directory, runtime.runtime_directory);
  if (bridge_path.empty()) {
    SetStatus("Unavailable: NR bridge not installed");
    REXLOG_WARN("Neural Rendering: bbport_nr_nvngx.dll is not installed");
    return false;
  }
  runtime.module = LoadLibraryW(bridge_path.c_str());
  if (!runtime.module) {
    SetStatus("Unavailable: NR bridge not installed");
    REXLOG_WARN("Neural Rendering: cannot load {}", bridge_path.string());
    return false;
  }
  const auto get_api = reinterpret_cast<BbNrGetApiFn>(GetProcAddress(runtime.module, "BbNrGetApi"));
  runtime.api = get_api ? get_api() : nullptr;
  if (!runtime.api || runtime.api->abi != BBPORT_NR_BRIDGE_ABI ||
      runtime.api->size < sizeof(BbNrApi)) {
    SetStatus("Unavailable: bridge ABI mismatch");
    REXLOG_WARN("Neural Rendering: {} has an incompatible bridge ABI", bridge_path.string());
    return false;
  }

  runtime.data_directory = executable_directory / L"neural-rendering-cache";
  std::error_code error;
  std::filesystem::create_directories(runtime.data_directory, error);
  if (error) {
    runtime.data_directory = executable_directory;
  }
  runtime.configured =
      runtime.api->Configure(runtime.runtime_path(), runtime.data_path(), &LogBridge) != 0;
  if (!runtime.configured) {
    SetStatus("Unavailable: bridge configuration failed");
  }
  return runtime.configured;
}

std::vector<const char*> ExtensionNames(const VkExtensionProperties* properties, uint32_t count) {
  std::vector<const char*> names;
  names.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (properties[i].extensionName[0]) {
      names.push_back(properties[i].extensionName);
    }
  }
  return names;
}

bool RuntimeSupportsOptimized() {
  static const bool supported = [] {
    const std::filesystem::path path = Runtime().runtime_directory / L"nvngx_dlssnr.dll";
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) {
      return false;
    }
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
      return false;
    }
    VS_FIXEDFILEINFO* version = nullptr;
    UINT version_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &version_size) ||
        !version || version_size < sizeof(VS_FIXEDFILEINFO)) {
      return false;
    }
    const std::array<unsigned, 4> actual = {
        HIWORD(version->dwFileVersionMS), LOWORD(version->dwFileVersionMS),
        HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS)};
    return actual >= std::array<unsigned, 4>{310, 9, 16, 0};
  }();
  return supported;
}

double Finite(double value, double fallback) {
  return std::isfinite(value) ? value : fallback;
}

struct Tuning {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t preset = 0;
  uint32_t style = 0;
  float intensity = 1.0f;
  float structure = 1.0f;
  float tone = 1.0f;

  bool operator==(const Tuning& other) const {
    return width == other.width && height == other.height && preset == other.preset &&
           style == other.style && intensity == other.intensity && structure == other.structure &&
           tone == other.tone;
  }
};

Tuning CurrentTuning(uint32_t width, uint32_t height) {
  Tuning tuning;
  tuning.width = width;
  tuning.height = height;
  tuning.preset = uint32_t(std::clamp(REXCVAR_GET(present_nr_preset), 0, 2));
  if (tuning.preset == 2 && !RuntimeSupportsOptimized()) {
    tuning.preset = 1;
  }
  tuning.style = uint32_t(std::clamp(REXCVAR_GET(present_nr_style), 0, 2));
  tuning.intensity = float(std::clamp(Finite(REXCVAR_GET(present_nr_intensity), 1.0), 0.0, 2.0));
  tuning.structure = float(std::clamp(Finite(REXCVAR_GET(present_nr_structure), 1.0), 0.0, 2.0));
  tuning.tone = float(std::clamp(Finite(REXCVAR_GET(present_nr_tone), 1.0), 0.0, 2.0));
  return tuning;
}

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct PushConstants {
  uint32_t width;
  uint32_t height;
  float motion_scale_x;
  float motion_scale_y;
};
static_assert(sizeof(PushConstants) == 16);

}  // namespace

namespace {
constexpr uint32_t kSetPerMode = 3;
}

struct VulkanNeuralRendering::Impl {
  explicit Impl(const VulkanDevice* device) : device(device) {
    if (!REXCVAR_GET(present_nr_enabled) || device->properties().vendorID != kNvidiaVendorId ||
        !PrepareRuntime()) {
      return;
    }
    const VulkanInstance* instance = device->vulkan_instance();
    const PFN_vkGetInstanceProcAddr get_instance_proc_addr =
        instance->functions().vkGetInstanceProcAddr;
    const auto get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get_instance_proc_addr(instance->instance(), "vkGetDeviceProcAddr"));
    model_initialized =
        runtime().api->Initialize(instance->instance(), device->physical_device(), device->device(),
                                  get_instance_proc_addr, get_device_proc_addr) != 0;
    if (!model_initialized) {
      SetStatus("Unavailable: model initialization failed");
      REXLOG_WARN(
          "Neural Rendering: model initialization failed; keeping the "
          "original frame and temporal upscaler");
      return;
    }
    if (!InitializeGpuObjects()) {
      initialized = false;
      Shutdown();
      return;
    }
    initialized = true;
    REXLOG_INFO("Neural Rendering: native Vulkan model initialized");
    SetStatus("Waiting for render settings");
  }

  ~Impl() { Shutdown(); }

  const BridgeRuntime& runtime() const { return Runtime(); }

  bool InitializeGpuObjects() {
    const VulkanDevice::Functions& dfn = device->functions();
    const VkDevice vk_device = device->device();

    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags =
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(vk_device, &pool_info, nullptr, &creation_pool) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo command_info = {};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = creation_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (dfn.vkAllocateCommandBuffers(vk_device, &command_info, &creation_command) != VK_SUCCESS) {
      return false;
    }
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // The first model upload has no prior submission to wait for. Starting
    // signaled lets the common wait-and-reset path safely prepare every
    // creation submission, including the first one.
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (dfn.vkCreateFence(vk_device, &fence_info, nullptr, &creation_fence) != VK_SUCCESS) {
      return false;
    }

    VkSamplerCreateInfo sampler_info = {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    if (dfn.vkCreateSampler(vk_device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
      return false;
    }

    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;
    if (dfn.vkCreateDescriptorSetLayout(vk_device, &layout_info, nullptr, &descriptor_layout) !=
        VK_SUCCESS) {
      return false;
    }

    VkPushConstantRange push_range = {};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pipeline_layout_info = {};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descriptor_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (dfn.vkCreatePipelineLayout(vk_device, &pipeline_layout_info, nullptr, &pipeline_layout) !=
        VK_SUCCESS) {
      return false;
    }

    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescriptorSetCount * 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kDescriptorSetCount},
    };
    VkDescriptorPoolCreateInfo descriptor_pool_info = {};
    descriptor_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptor_pool_info.maxSets = kDescriptorSetCount;
    descriptor_pool_info.poolSizeCount = 2;
    descriptor_pool_info.pPoolSizes = sizes;
    if (dfn.vkCreateDescriptorPool(vk_device, &descriptor_pool_info, nullptr, &descriptor_pool) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetLayout layouts[kDescriptorSetCount];
    std::fill(std::begin(layouts), std::end(layouts), descriptor_layout);
    VkDescriptorSetAllocateInfo set_info = {};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = descriptor_pool;
    set_info.descriptorSetCount = kDescriptorSetCount;
    set_info.pSetLayouts = layouts;
    if (dfn.vkAllocateDescriptorSets(vk_device, &set_info, descriptor_sets) != VK_SUCCESS) {
      return false;
    }

    color_pipeline = CreatePipeline(shaders::neural_rendering_color_cs,
                                    sizeof(shaders::neural_rendering_color_cs));
    motion_pipeline = CreatePipeline(shaders::neural_rendering_motion_cs,
                                     sizeof(shaders::neural_rendering_motion_cs));
    pack_pipeline = CreatePipeline(shaders::neural_rendering_pack_cs,
                                   sizeof(shaders::neural_rendering_pack_cs));
    return color_pipeline && motion_pipeline && pack_pipeline;
  }

  VkPipeline CreatePipeline(const uint32_t* code, size_t size) {
    const VulkanDevice::Functions& dfn = device->functions();
    VkShaderModule module = util::CreateShaderModule(device, code, size);
    if (!module) {
      return VK_NULL_HANDLE;
    }
    VkPipeline pipeline = util::CreateComputePipeline(device, pipeline_layout, module);
    dfn.vkDestroyShaderModule(device->device(), module, nullptr);
    return pipeline;
  }

  bool CreateImage(Image& target, VkFormat format, uint32_t width, uint32_t height) {
    VkImageCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!util::CreateDedicatedAllocationImage(device, info, util::MemoryPurpose::kDeviceLocal,
                                              target.image, target.memory)) {
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = target.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = util::InitializeSubresourceRange();
    if (device->functions().vkCreateImageView(device->device(), &view_info, nullptr,
                                              &target.view) != VK_SUCCESS) {
      DestroyImage(target);
      return false;
    }
    target.format = format;
    target.width = width;
    target.height = height;
    return true;
  }

  void DestroyImage(Image& target) {
    const VulkanDevice::Functions& dfn = device->functions();
    util::DestroyAndNullHandle(dfn.vkDestroyImageView, device->device(), target.view);
    util::DestroyAndNullHandle(dfn.vkDestroyImage, device->device(), target.image);
    util::DestroyAndNullHandle(dfn.vkFreeMemory, device->device(), target.memory);
    target = {};
  }

  bool EnsureImages(const Tuning& tuning) {
    if (color_input.width == tuning.width && color_input.height == tuning.height) {
      return true;
    }
    DestroyImages();
    return CreateImage(color_input, VK_FORMAT_R16G16B16A16_SFLOAT, tuning.width, tuning.height) &&
           CreateImage(nr_output, VK_FORMAT_R16G16B16A16_SFLOAT, tuning.width, tuning.height) &&
           CreateImage(motion_pixels, VK_FORMAT_R16G16_SFLOAT, tuning.width, tuning.height) &&
           CreateImage(processed_source, VK_FORMAT_A2B10G10R10_UNORM_PACK32, tuning.width,
                       tuning.height);
  }

  void DestroyImages() {
    DestroyImage(color_input);
    DestroyImage(nr_output);
    DestroyImage(motion_pixels);
    DestroyImage(processed_source);
  }

  void Transition(Image& image, VkImageLayout new_layout, VkPipelineStageFlags src_stage,
                  VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                  VkAccessFlags dst_access, VkCommandBuffer command) {
    if (image.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
      src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      src_access = 0;
    }
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = image.layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = util::InitializeSubresourceRange();
    device->functions().vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, nullptr, 0,
                                             nullptr, 1, &barrier);
    image.layout = new_layout;
  }

  bool SubmitPassCreation(const Tuning& tuning) {
    const VulkanDevice::Functions& dfn = device->functions();
    const VkDevice vk_device = device->device();
    if (creation_fence != VK_NULL_HANDLE) {
      if (dfn.vkWaitForFences(vk_device, 1, &creation_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return false;
      }
      dfn.vkResetFences(vk_device, 1, &creation_fence);
    }
    if (dfn.vkResetCommandPool(vk_device, creation_pool, 0) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(creation_command, &begin_info) != VK_SUCCESS) {
      return false;
    }
    const BbNrTuning bridge_tuning = {
        tuning.width,
        tuning.height,
        tuning.preset,
        tuning.style,
        tuning.intensity,
        tuning.structure,
        tuning.tone,
        1.0f,
        0,
        0,
    };
    if (!runtime().api->CreatePass(creation_command, 0, &bridge_tuning)) {
      dfn.vkEndCommandBuffer(creation_command);
      return false;
    }
    if (dfn.vkEndCommandBuffer(creation_command) != VK_SUCCESS) {
      runtime().api->ReleasePass(0);
      return false;
    }
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &creation_command;
    const VulkanDevice::Queue::Acquisition queue =
        device->AcquireQueue(device->queue_family_graphics_compute(), 0);
    if (dfn.vkQueueSubmit(queue.queue(), 1, &submit_info, creation_fence) != VK_SUCCESS) {
      runtime().api->ReleasePass(0);
      return false;
    }
    creation_submitted = true;
    pass_ready = false;
    return true;
  }

  bool EnsurePass(const Tuning& tuning) {
    if (pass_built) {
      if (!pass_ready && creation_submitted && creation_fence != VK_NULL_HANDLE) {
        const VkResult status =
            device->functions().vkGetFenceStatus(device->device(), creation_fence);
        if (status == VK_SUCCESS) {
          pass_ready = runtime().api->ReadyPass(0) != 0;
          if (pass_ready) {
            REXLOG_INFO("Neural Rendering: model upload complete at {}x{}", tuning.width,
                        tuning.height);
            SetStatus("Ready");
          }
        }
      }
      return pass_ready;
    }

    if (!EnsureImages(tuning)) {
      SetStatus("Unavailable: NR allocation failed");
      REXLOG_WARN("Neural Rendering: render-resolution FP16 allocations failed");
      failed = true;
      return false;
    }
    pass_tuning = tuning;
    if (!SubmitPassCreation(tuning)) {
      SetStatus("Unavailable: model creation failed");
      REXLOG_WARN("Neural Rendering: model creation submission failed");
      failed = true;
      return false;
    }
    pass_built = true;
    reset_history = true;
    SetStatus("Loading neural-rendering weights");
    return false;
  }

  void ReleasePass() {
    if (creation_submitted && creation_fence != VK_NULL_HANDLE) {
      device->functions().vkWaitForFences(device->device(), 1, &creation_fence, VK_TRUE,
                                          UINT64_MAX);
    }
    if (pass_built && runtime().api) {
      runtime().api->ReleasePass(0);
    }
    pass_built = false;
    pass_ready = false;
    creation_submitted = false;
  }

  bool ReconfigureIfNeeded(const Tuning& tuning) {
    if (pass_built && pass_tuning == tuning) {
      return true;
    }
    if (pass_built) {
      ReleasePass();
    }
    if (color_input.width != tuning.width || color_input.height != tuning.height) {
      DestroyImages();
    }
    failed = false;
    return true;
  }

  void UpdateDescriptors(VkDescriptorSet set, VkImageView sampled, VkImageLayout sampled_layout,
                         VkImageView auxiliary, VkImageLayout auxiliary_layout,
                         VkImageView storage) {
    VkDescriptorImageInfo sampled_info = {sampler, sampled, sampled_layout};
    VkDescriptorImageInfo auxiliary_info = {sampler, auxiliary, auxiliary_layout};
    VkDescriptorImageInfo storage_info = {VK_NULL_HANDLE, storage, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType =
          i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].pImageInfo = i == 2 ? &storage_info : i == 1 ? &auxiliary_info : &sampled_info;
    }
    device->functions().vkUpdateDescriptorSets(device->device(), 3, writes, 0, nullptr);
  }

  void RunConversion(VkCommandBuffer command, VkPipeline pipeline, VkDescriptorSet set,
                     uint32_t width, uint32_t height, float scale_x = 0.0f, float scale_y = 0.0f) {
    const VulkanDevice::Functions& dfn = device->functions();
    const PushConstants constants = {width, height, scale_x, scale_y};
    dfn.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    dfn.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
                                &set, 0, nullptr);
    dfn.vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(constants), &constants);
    dfn.vkCmdDispatch(command, (width + kDispatchGroupSize - 1) / kDispatchGroupSize,
                      (height + kDispatchGroupSize - 1) / kDispatchGroupSize, 1);
  }

  bool Dispatch(VkCommandBuffer command, uint32_t descriptor_set_index,
                const VulkanUpscalerImage& color, const VulkanUpscalerImage& motion_vectors,
                const VulkanUpscalerImage& depth, bool depth_inverted, bool reset,
                float motion_scale_x, float motion_scale_y, VulkanUpscalerImage& processed) {
    if (!initialized || failed) {
      return false;
    }
    if (color.format != VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
        motion_vectors.format != VK_FORMAT_R16G16_SFLOAT || depth.format != VK_FORMAT_R32_SFLOAT ||
        !color.width || !color.height) {
      SetStatus("Unavailable: unsupported frame inputs");
      return false;
    }
    const Tuning tuning = CurrentTuning(color.width, color.height);
    ReconfigureIfNeeded(tuning);
    if (!EnsurePass(tuning)) {
      return false;
    }

    const uint32_t frame_set = descriptor_set_index % kSetPerMode;
    VkDescriptorSet color_set = descriptor_sets[frame_set];
    VkDescriptorSet motion_set = descriptor_sets[kSetPerMode + frame_set];
    VkDescriptorSet pack_set = descriptor_sets[kSetPerMode * 2 + frame_set];

    constexpr VkPipelineStageFlags kCompute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    constexpr VkAccessFlags kRead = VK_ACCESS_SHADER_READ_BIT;
    constexpr VkAccessFlags kWrite = VK_ACCESS_SHADER_WRITE_BIT;
    Transition(color_input, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               kRead | kWrite, kCompute, kWrite, command);
    Transition(motion_pixels, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               kRead | kWrite, kCompute, kWrite, command);
    Transition(nr_output, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               kRead | kWrite, kCompute, kWrite, command);
    Transition(processed_source, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
               kRead | kWrite, kCompute, kWrite, command);

    UpdateDescriptors(color_set, color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color.view,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color_input.view);
    RunConversion(command, color_pipeline, color_set, color.width, color.height);
    Transition(color_input, VK_IMAGE_LAYOUT_GENERAL, kCompute, kWrite, kCompute, kRead, command);

    UpdateDescriptors(motion_set, motion_vectors.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      motion_vectors.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      motion_pixels.view);
    RunConversion(command, motion_pipeline, motion_set, color.width, color.height, motion_scale_x,
                  motion_scale_y);
    Transition(motion_pixels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute, kWrite, kCompute,
               kRead, command);

    const VkImageSubresourceRange range = util::InitializeSubresourceRange();
    const BbNrImage bridge_color = {color_input.image,  color_input.view, range,
                                    color_input.format, color.width,      color.height};
    const BbNrImage bridge_motion = {motion_pixels.image,  motion_pixels.view, range,
                                     motion_pixels.format, color.width,        color.height};
    const BbNrImage bridge_depth = {depth.image,  depth.view,  range,
                                    depth.format, color.width, color.height};
    const BbNrImage bridge_output = {nr_output.image,  nr_output.view, range,
                                     nr_output.format, color.width,    color.height};
    BbNrEvaluate evaluate = {};
    evaluate.color = bridge_color;
    evaluate.motion = bridge_motion;
    evaluate.depth = bridge_depth;
    evaluate.output = bridge_output;
    evaluate.depth_inverted = depth_inverted ? 1u : 0u;
    evaluate.reset = (reset || reset_history) ? 1u : 0u;
    evaluate.mv_scale_x = 1.0f;
    evaluate.mv_scale_y = 1.0f;
    if (!runtime().api->EvaluatePass(command, 0, &evaluate)) {
      SetStatus("Unavailable: evaluation failed");
      REXLOG_WARN("Neural Rendering: evaluation failed; retaining the original frame");
      failed = true;
      return false;
    }
    Transition(nr_output, VK_IMAGE_LAYOUT_GENERAL, kCompute, kWrite, kCompute, kRead, command);

    UpdateDescriptors(pack_set, nr_output.view, VK_IMAGE_LAYOUT_GENERAL, color.view,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, processed_source.view);
    RunConversion(command, pack_pipeline, pack_set, color.width, color.height);
    Transition(processed_source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kCompute, kWrite,
               kCompute, kRead, command);

    processed = {processed_source.image,  processed_source.view,
                 processed_source.format, processed_source.width,
                 processed_source.height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
    reset_history = false;
    SetStatus("Active");
    return true;
  }

  void Shutdown() {
    if (device) {
      ReleasePass();
      DestroyImages();
      const VulkanDevice::Functions& dfn = device->functions();
      const VkDevice vk_device = device->device();
      util::DestroyAndNullHandle(dfn.vkDestroyPipeline, vk_device, color_pipeline);
      util::DestroyAndNullHandle(dfn.vkDestroyPipeline, vk_device, motion_pipeline);
      util::DestroyAndNullHandle(dfn.vkDestroyPipeline, vk_device, pack_pipeline);
      util::DestroyAndNullHandle(dfn.vkDestroySampler, vk_device, sampler);
      util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, vk_device, descriptor_pool);
      util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, vk_device, pipeline_layout);
      util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, vk_device, descriptor_layout);
      util::DestroyAndNullHandle(dfn.vkDestroyFence, vk_device, creation_fence);
      if (creation_pool) {
        dfn.vkDestroyCommandPool(vk_device, creation_pool, nullptr);
        creation_pool = VK_NULL_HANDLE;
        creation_command = VK_NULL_HANDLE;
      }
    }
    if ((initialized || model_initialized) && Runtime().api) {
      Runtime().api->Shutdown();
    }
    initialized = false;
    model_initialized = false;
  }

  const VulkanDevice* device = nullptr;
  bool initialized = false;
  bool model_initialized = false;
  bool failed = false;
  bool pass_built = false;
  bool pass_ready = false;
  bool creation_submitted = false;
  bool reset_history = true;
  Tuning pass_tuning;

  VkCommandPool creation_pool = VK_NULL_HANDLE;
  VkCommandBuffer creation_command = VK_NULL_HANDLE;
  VkFence creation_fence = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_sets[kDescriptorSetCount] = {};
  VkPipeline color_pipeline = VK_NULL_HANDLE;
  VkPipeline motion_pipeline = VK_NULL_HANDLE;
  VkPipeline pack_pipeline = VK_NULL_HANDLE;
  Image color_input;
  Image nr_output;
  Image motion_pixels;
  Image processed_source;
};

#else

struct VulkanNeuralRendering::Impl {
  explicit Impl(const VulkanDevice*) {}
  bool initialized = false;
};

#endif  // REX_PLATFORM_WIN32

VulkanNeuralRendering::VulkanNeuralRendering(const VulkanDevice* device)
    : impl_(std::make_unique<Impl>(device)) {}

VulkanNeuralRendering::~VulkanNeuralRendering() = default;

bool VulkanNeuralRendering::initialized() const {
  return impl_->initialized;
}

bool VulkanNeuralRendering::RequiresGpuWait(uint32_t width, uint32_t height) const {
#if REX_PLATFORM_WIN32
  if (!impl_->initialized) {
    return false;
  }
  const Tuning tuning = CurrentTuning(width, height);
  return (impl_->pass_built && !(impl_->pass_tuning == tuning)) ||
         (impl_->color_input.width &&
          (impl_->color_input.width != width || impl_->color_input.height != height));
#else
  (void)width;
  (void)height;
  return false;
#endif
}

#if REX_PLATFORM_WIN32

bool VulkanNeuralRendering::Dispatch(VkCommandBuffer command_buffer, uint32_t descriptor_set_index,
                                     const VulkanUpscalerImage& color,
                                     const VulkanUpscalerImage& motion_vectors,
                                     const VulkanUpscalerImage& depth, bool depth_inverted,
                                     bool reset, float motion_scale_x, float motion_scale_y,
                                     VulkanUpscalerImage& processed) {
  return impl_->Dispatch(command_buffer, descriptor_set_index, color, motion_vectors, depth,
                         depth_inverted, reset, motion_scale_x, motion_scale_y, processed);
}

#else

bool VulkanNeuralRendering::Dispatch(VkCommandBuffer, uint32_t, const VulkanUpscalerImage&,
                                     const VulkanUpscalerImage&, const VulkanUpscalerImage&, bool,
                                     bool, float, float, VulkanUpscalerImage&) {
  return false;
}

#endif

std::vector<const char*> GetVulkanNeuralRenderingInstanceExtensions() {
#if REX_PLATFORM_WIN32
  if (!PrepareRuntime()) {
    return {};
  }
  uint32_t count = 0;
  const VkExtensionProperties* properties = nullptr;
  if (!Runtime().api->InstanceExtensions(&count, &properties)) {
    return {};
  }
  return ExtensionNames(properties, count);
#else
  return {};
#endif
}

std::vector<const char*> GetVulkanNeuralRenderingDeviceExtensions(VkInstance instance,
                                                                  VkPhysicalDevice physical_device,
                                                                  uint32_t vendor_id) {
#if REX_PLATFORM_WIN32
  if (vendor_id != kNvidiaVendorId || !PrepareRuntime()) {
    return {};
  }
  uint32_t count = 0;
  const VkExtensionProperties* properties = nullptr;
  if (!Runtime().api->DeviceExtensions(instance, physical_device, &count, &properties)) {
    return {};
  }
  return ExtensionNames(properties, count);
#else
  (void)instance;
  (void)physical_device;
  (void)vendor_id;
  return {};
#endif
}

bool VulkanNeuralRenderingRequiresBufferDeviceAddress() {
#if REX_PLATFORM_WIN32
  return REXCVAR_GET(present_nr_enabled) && PrepareRuntime();
#else
  return false;
#endif
}

bool IsVulkanNeuralRenderingInstalled() {
#if REX_PLATFORM_WIN32
  const std::filesystem::path executable_directory = ModuleDirectory();
  const std::filesystem::path runtime_directory = FindRuntimeDirectory(executable_directory);
  return !runtime_directory.empty() && !FindBridge(executable_directory, runtime_directory).empty();
#else
  return false;
#endif
}

std::string GetVulkanNeuralRenderingStatus() {
  const std::lock_guard lock(g_status_mutex);
  return g_status;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
