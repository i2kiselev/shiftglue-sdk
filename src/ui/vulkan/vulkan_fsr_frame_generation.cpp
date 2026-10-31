#include "ui/vulkan/vulkan_fsr_frame_generation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include <ffx_vk_fsr3_3_1_5_bridge.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/util.h>

namespace rex {
namespace ui {
namespace vulkan {

namespace {

constexpr VkImageUsageFlags kReadUsage = VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kColorInputUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
constexpr VkImageUsageFlags kColorOutputUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageUsageFlags kConversionUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkFormat kProviderColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageLayout kProviderInputLayout = VK_IMAGE_LAYOUT_GENERAL;
constexpr VkImageLayout kProviderOutputLayout = VK_IMAGE_LAYOUT_GENERAL;

std::mutex g_status_mutex;
std::string g_status = "Frame generation: FSR not initialized";

void SetStatus(std::string status, bool log = false) {
  std::string message = std::move(status);
  {
    std::lock_guard lock(g_status_mutex);
    g_status = message;
  }
  if (log) {
    REXLOG_INFO("{}", message);
  }
}

bool IsFinite(float value) {
  return std::isfinite(value);
}

bool IsFinitePositive(float value) {
  return IsFinite(value) && value > 0.0f;
}

bool IsFiniteVector(const float value[3]) {
  return IsFinite(value[0]) && IsFinite(value[1]) && IsFinite(value[2]);
}

bool IsFiniteMatrix(const float value[16]) {
  return std::all_of(value, value + 16, IsFinite);
}

bool IsSupportedColorFormat(VkFormat format) {
  return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R16G16B16A16_SFLOAT ||
         format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
}

bool IsSupportedMotionFormat(VkFormat format) {
  return format == VK_FORMAT_R16G16_SFLOAT || format == VK_FORMAT_R16G16B16A16_SFLOAT;
}

const char* ResultName(FfxVkFsr3_3_1_6FrameGenerationResult result) {
  switch (result) {
    case FFX_VK_FSR3_3_1_6_FRAMEGEN_OK:
      return "ok";
    case FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT:
      return "invalid argument";
    case FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_OUT_OF_MEMORY:
      return "out of memory";
    case FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_BACKEND:
      return "Vulkan backend error";
    case FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_IN_FLIGHT:
      return "frame already in flight";
  }
  return "unknown error";
}

FfxVkFsr3_3_1_6FrameGenerationImage ToBridgeImage(const VulkanFsrFrameGeneration::Image& image,
                                                  VkImageUsageFlags required_usage) {
  FfxVkFsr3_3_1_6FrameGenerationImage result{};
  result.image = image.image;
  result.format = image.format;
  result.width = image.width;
  result.height = image.height;
  result.usage = required_usage;
  return result;
}

bool ValidateImage(const VulkanFsrFrameGeneration::Image& image, VkFormat format, uint32_t width,
                   uint32_t height, VkImageUsageFlags required_usage) {
  return image.image != VK_NULL_HANDLE && image.view != VK_NULL_HANDLE && image.format == format &&
         image.width == width && image.height == height &&
         (image.usage & required_usage) == required_usage;
}

void RecordImageTransition(const VulkanDevice::Functions& functions, VkCommandBuffer command_buffer,
                           VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                           VkPipelineStageFlags source_stage, VkAccessFlags source_access,
                           VkPipelineStageFlags destination_stage,
                           VkAccessFlags destination_access) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.srcAccessMask = source_access;
  barrier.dstAccessMask = destination_access;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  functions.vkCmdPipelineBarrier(command_buffer, source_stage, destination_stage, 0, 0, nullptr, 0,
                                 nullptr, 1, &barrier);
}

}  // namespace

class VulkanFsrFrameGeneration::Impl {
 public:
  explicit Impl(const VulkanDevice& device) : device_(device) { QueryCapabilities(); }

  ~Impl() { Shutdown(); }

  bool Prepare(uint32_t width, uint32_t height, VkFormat format) {
    if (width == 0 || height == 0 || !IsSupportedColorFormat(format)) {
      SetStatus("Frame generation: FSR unavailable (unsupported output format)");
      REXLOG_WARN("FSR frame generation: unsupported display configuration {}x{} format {}", width,
                  height, static_cast<int>(format));
      ShutdownContext();
      ShutdownConversionImages();
      prepared_ = false;
      return false;
    }

    const auto max_dimension = device_.properties().maxImageDimension2D;
    if (width > max_dimension || height > max_dimension) {
      SetStatus("Frame generation: FSR unavailable (output extent exceeds device limit)");
      REXLOG_WARN("FSR frame generation: {}x{} exceeds maxImageDimension2D {}", width, height,
                  max_dimension);
      ShutdownContext();
      ShutdownConversionImages();
      prepared_ = false;
      return false;
    }

    if (context_ && input_color_.image && output_color_.image && prepared_ && width_ == width &&
        height_ == height && color_format_ == format) {
      return true;
    }

    if (has_unretired_) {
      REXLOG_WARN(
          "FSR frame generation: cannot recreate while frame {} is not retired "
          "(retired through {})",
          highest_recorded_frame_id_, highest_retired_frame_id_);
      return false;
    }

    ShutdownContext();
    ShutdownConversionImages();
    width_ = width;
    height_ = height;
    color_format_ = format;
    prepared_ = false;
    needs_reset_ = true;

    if (!capability_available_) {
      SetStatus(std::string("Frame generation: FSR unavailable (") + capability_reason_ + ")");
      REXLOG_WARN("FSR frame generation: unavailable: {}", capability_reason_);
      return false;
    }

    constexpr VkFormatFeatureFlags kBlitFeatures =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if (!HasFormatFeatures(format, kBlitFeatures) ||
        !HasFormatFeatures(kProviderColorFormat, kBlitFeatures |
                                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                     VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
      capability_reason_ = "color formats do not support required transfer and FFX uses";
      SetStatus(std::string("Frame generation: FSR unavailable (") + capability_reason_ + ")");
      REXLOG_WARN("FSR frame generation: {}", capability_reason_);
      return false;
    }

    if (!CreateConversionImages(width, height)) {
      SetStatus("Frame generation: FSR unavailable (conversion image creation failed)");
      return false;
    }

    FfxVkFsr3_3_1_6FrameGenerationCreateInfo create_info{};
    create_info.physicalDevice = device_.physical_device();
    create_info.device = device_.device();
    create_info.maxRenderWidth = width;
    create_info.maxRenderHeight = height;
    create_info.displayWidth = width;
    create_info.displayHeight = height;
    create_info.colorFormat = kProviderColorFormat;

    FfxVkFsr3_3_1_6FrameGenerationContext* context = nullptr;
    const auto result = ffxVkFsr3_3_1_6FrameGenerationContextCreate(&create_info, &context);
    if (result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK || context == nullptr) {
      SetStatus("Frame generation: FSR unavailable (context creation failed)");
      REXLOG_WARN("FSR frame generation: provider context creation failed for {}x{}: {}", width,
                  height, ResultName(result));
      ShutdownConversionImages();
      return false;
    }

    context_ = context;
    prepared_ = true;
    highest_recorded_frame_id_ = 0;
    highest_retired_frame_id_ = 0;
    has_recorded_ = false;
    has_unretired_ = false;

    FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
    if (ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(context, &usage) ==
        FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
      REXLOG_INFO(
          "FSR frame generation: FSR 3.1.6 ready for {}x{}, color format {} "
          "({} MiB effect-owned)",
          width, height, static_cast<int>(format),
          static_cast<double>(usage.totalUsageInBytes) / (1024.0 * 1024.0));
      SetStatus("Frame generation: FSR 3.1.6 available, waiting for the first frame");
    } else {
      REXLOG_INFO("FSR frame generation: FSR 3.1.6 ready for {}x{}, color format {}", width, height,
                  static_cast<int>(format));
      SetStatus("Frame generation: FSR 3.1.6 available, waiting for the first frame");
    }
    return true;
  }

  bool Record(VkCommandBuffer command_buffer, const VulkanFsrFrameGeneration::Input& input,
              const VulkanFsrFrameGeneration::Output& output, uint64_t frame_id, bool reset) {
    if (!context_ || !prepared_) {
      REXLOG_WARN("FSR frame generation: Record called before a successful Prepare");
      return false;
    }
    if (command_buffer == VK_NULL_HANDLE ||
        (has_recorded_ && frame_id <= highest_recorded_frame_id_)) {
      REXLOG_WARN(
          "FSR frame generation: invalid command buffer or non-monotonic frame ID {} "
          "(last {})",
          frame_id, highest_recorded_frame_id_);
      return false;
    }
    if (!ValidateImage(input.color, color_format_, width_, height_, kColorInputUsage) ||
        !ValidateImage(input.depth, VK_FORMAT_R32_SFLOAT, input.depth.width, input.depth.height,
                       kReadUsage) ||
        !ValidateImage(input.motion_vectors, input.motion_vectors.format, input.depth.width,
                       input.depth.height, kReadUsage) ||
        !IsSupportedMotionFormat(input.motion_vectors.format) ||
        !ValidateImage(output, color_format_, width_, height_, kColorOutputUsage)) {
      SetStatus("Frame generation: FSR unavailable (invalid image contract)");
      REXLOG_WARN(
          "FSR frame generation: invalid image contract for frame {} "
          "(display {}x{}, render {}x{})",
          frame_id, width_, height_, input.depth.width, input.depth.height);
      return false;
    }
    if (input.depth.width == 0 || input.depth.height == 0 || input.depth.width > width_ ||
        input.depth.height > height_) {
      REXLOG_WARN("FSR frame generation: invalid render extent {}x{} for display {}x{}",
                  input.depth.width, input.depth.height, width_, height_);
      return false;
    }
    if (input.color.image == input.depth.image || input.color.image == input.motion_vectors.image ||
        input.depth.image == input.motion_vectors.image || output.image == input.color.image ||
        output.image == input.depth.image || output.image == input.motion_vectors.image) {
      REXLOG_WARN("FSR frame generation: input/output image aliasing is not supported");
      return false;
    }

    const auto& camera = input.camera;
    if (!IsFinitePositive(camera.near_plane) || !IsFinitePositive(camera.far_plane) ||
        camera.far_plane <= camera.near_plane || !IsFinitePositive(camera.fov_y) ||
        !IsFinite(camera.jitter_x) || !IsFinite(camera.jitter_y) ||
        !IsFiniteVector(camera.position) || !IsFiniteVector(camera.up) ||
        !IsFiniteVector(camera.right) || !IsFiniteVector(camera.forward) ||
        !camera.frame_generation_matrices_valid || !IsFiniteMatrix(camera.view_to_clip) ||
        !IsFiniteMatrix(camera.clip_to_view) || !IsFiniteMatrix(camera.clip_to_previous) ||
        !IsFiniteMatrix(camera.previous_to_clip) || !IsFinitePositive(input.frame_delta_ms)) {
      REXLOG_WARN("FSR frame generation: invalid camera or frame timing for frame {}", frame_id);
      return false;
    }

    const auto& functions = device_.functions();
    constexpr VkAccessFlags kProducedAccess = VK_ACCESS_SHADER_WRITE_BIT |
                                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                              VK_ACCESS_TRANSFER_WRITE_BIT;

    RecordImageTransition(functions, command_buffer, input.color.image, VK_IMAGE_LAYOUT_GENERAL,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          kProducedAccess, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT);
    RecordImageTransition(functions, command_buffer, input_color_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit input_blit{};
    input_blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    input_blit.srcOffsets[1] = {static_cast<int32_t>(width_), static_cast<int32_t>(height_), 1};
    input_blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    input_blit.dstOffsets[1] = {static_cast<int32_t>(width_), static_cast<int32_t>(height_), 1};
    functions.vkCmdBlitImage(
        command_buffer, input.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, input_color_.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &input_blit, VK_FILTER_NEAREST);
    RecordImageTransition(functions, command_buffer, input_color_.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kProviderInputLayout,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    RecordImageTransition(functions, command_buffer, input.color.image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, kProducedAccess);
    RecordImageTransition(functions, command_buffer, output_color_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          kProviderOutputLayout, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    VkMemoryBarrier pre_barrier{};
    pre_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    pre_barrier.srcAccessMask = kProducedAccess;
    pre_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    functions.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &pre_barrier, 0,
                                   nullptr, 0, nullptr);

    const bool effective_reset = reset || camera.cut || needs_reset_;
    needs_reset_ = false;
    highest_recorded_frame_id_ = frame_id;
    has_recorded_ = true;
    has_unretired_ = true;

    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{};
    prepare.commandBuffer = command_buffer;
    prepare.color = {};
    prepare.color.image = input_color_.image;
    prepare.color.format = kProviderColorFormat;
    prepare.color.width = width_;
    prepare.color.height = height_;
    prepare.color.layout = kProviderInputLayout;
    prepare.color.usage = kConversionUsage;
    prepare.depth = ToBridgeImage(input.depth, kReadUsage);
    prepare.motionVectors = ToBridgeImage(input.motion_vectors, kReadUsage);
    prepare.motionVectors.layout = kProviderInputLayout;
    prepare.renderWidth = input.depth.width;
    prepare.renderHeight = input.depth.height;
    prepare.depth.layout = kProviderInputLayout;
    prepare.jitterOffsetX = camera.jitter_x;
    prepare.jitterOffsetY = camera.jitter_y;
    prepare.motionVectorScaleX = static_cast<float>(input.depth.width);
    prepare.motionVectorScaleY = static_cast<float>(input.depth.height);
    prepare.frameTimeMilliseconds = input.frame_delta_ms;
    prepare.minLuminance = 0.0f;
    prepare.maxLuminance = 1.0f;
    prepare.transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
    prepare.cameraNear = camera.near_plane;
    prepare.cameraFar = camera.far_plane;
    prepare.viewSpaceToMeters = 1.0f;
    prepare.cameraVerticalFovRadians = camera.fov_y;
    std::memcpy(prepare.cameraPosition, camera.position, sizeof(prepare.cameraPosition));
    std::memcpy(prepare.cameraUp, camera.up, sizeof(prepare.cameraUp));
    std::memcpy(prepare.cameraRight, camera.right, sizeof(prepare.cameraRight));
    std::memcpy(prepare.cameraForward, camera.forward, sizeof(prepare.cameraForward));
    prepare.frameId = frame_id;
    prepare.reset = effective_reset ? VK_TRUE : VK_FALSE;

    const auto prepare_result =
        ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context_, &prepare);
    if (prepare_result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
      needs_reset_ = true;
      SetStatus("Frame generation: FSR unavailable (prepare failed)");
      REXLOG_WARN("FSR frame generation: prepare failed for frame {}: {}", frame_id,
                  ResultName(prepare_result));
      return false;
    }

    FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{};
    dispatch.commandBuffer = command_buffer;
    dispatch.color = prepare.color;
    dispatch.output = {};
    dispatch.output.image = output_color_.image;
    dispatch.output.format = kProviderColorFormat;
    dispatch.output.width = width_;
    dispatch.output.height = height_;
    dispatch.output.layout = kProviderOutputLayout;
    dispatch.output.usage = kConversionUsage;
    dispatch.displayWidth = width_;
    dispatch.displayHeight = height_;
    dispatch.frameTimeMilliseconds = input.frame_delta_ms;
    dispatch.cameraNear = camera.near_plane;
    dispatch.cameraFar = camera.far_plane;
    dispatch.viewSpaceToMeters = 1.0f;
    dispatch.cameraVerticalFovRadians = camera.fov_y;
    dispatch.minLuminance = 0.0f;
    dispatch.maxLuminance = 1.0f;
    dispatch.transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
    dispatch.frameId = frame_id;
    dispatch.reset = effective_reset ? VK_TRUE : VK_FALSE;

    const auto dispatch_result =
        ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context_, &dispatch);
    if (dispatch_result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
      needs_reset_ = true;
      SetStatus("Frame generation: FSR unavailable (dispatch failed)");
      REXLOG_WARN("FSR frame generation: dispatch failed for frame {}: {}", frame_id,
                  ResultName(dispatch_result));
      return false;
    }

    RecordImageTransition(functions, command_buffer, output_color_.image, kProviderOutputLayout,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    RecordImageTransition(functions, command_buffer, output.image, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit output_blit{};
    output_blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    output_blit.srcOffsets[1] = {static_cast<int32_t>(width_), static_cast<int32_t>(height_), 1};
    output_blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    output_blit.dstOffsets[1] = {static_cast<int32_t>(width_), static_cast<int32_t>(height_), 1};
    functions.vkCmdBlitImage(
        command_buffer, output_color_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &output_blit, VK_FILTER_NEAREST);
    RecordImageTransition(functions, command_buffer, output_color_.image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, kProducedAccess);
    RecordImageTransition(
        functions, command_buffer, output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    SetStatus("Frame generation: FSR active, 2x");
    return true;
  }

  void Retire(uint64_t completed_frame_id) {
    if (completed_frame_id < highest_retired_frame_id_ || context_ == nullptr) {
      return;
    }
    const auto result =
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context_, completed_frame_id);
    if (result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
      REXLOG_WARN("FSR frame generation: retire through frame {} failed: {}", completed_frame_id,
                  ResultName(result));
      return;
    }
    highest_retired_frame_id_ = completed_frame_id;
    if (completed_frame_id >= highest_recorded_frame_id_) {
      has_unretired_ = false;
    }
  }

  [[nodiscard]] bool Available() const {
    return context_ != nullptr && input_color_.image && output_color_.image && prepared_;
  }

  void Shutdown() {
    const bool had_context = context_ != nullptr;
    ShutdownContext();
    ShutdownConversionImages();
    prepared_ = false;
    needs_reset_ = true;
    width_ = 0;
    height_ = 0;
    color_format_ = VK_FORMAT_UNDEFINED;
    highest_recorded_frame_id_ = 0;
    highest_retired_frame_id_ = 0;
    has_recorded_ = false;
    has_unretired_ = false;
    SetStatus(had_context
                  ? "Frame generation: FSR shut down"
                  : std::string("Frame generation: FSR unavailable (") + capability_reason_ + ")");
  }

 private:
  struct ConversionImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  bool HasFormatFeatures(VkFormat format, VkFormatFeatureFlags required) const {
    VkFormatProperties properties{};
    device_.vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
        device_.physical_device(), format, &properties);
    return (properties.optimalTilingFeatures & required) == required;
  }

  bool CreateConversionImage(uint32_t width, uint32_t height, ConversionImage& image) {
    VkImageCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    create_info.imageType = VK_IMAGE_TYPE_2D;
    create_info.format = kProviderColorFormat;
    create_info.extent = {width, height, 1};
    create_info.mipLevels = 1;
    create_info.arrayLayers = 1;
    create_info.samples = VK_SAMPLE_COUNT_1_BIT;
    create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    create_info.usage = kConversionUsage;
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!util::CreateDedicatedAllocationImage(
            &device_, create_info, util::MemoryPurpose::kDeviceLocal, image.image, image.memory)) {
      REXLOG_WARN("FSR frame generation: failed to create {}x{} RGBA16 conversion image", width,
                  height);
      return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kProviderColorFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkResult view_result =
        device_.functions().vkCreateImageView(device_.device(), &view_info, nullptr, &image.view);
    if (view_result != VK_SUCCESS) {
      REXLOG_WARN("FSR frame generation: failed to create RGBA16 conversion image view: {}",
                  static_cast<int>(view_result));
      device_.functions().vkDestroyImage(device_.device(), image.image, nullptr);
      device_.functions().vkFreeMemory(device_.device(), image.memory, nullptr);
      image = {};
      return false;
    }
    return true;
  }

  bool CreateConversionImages(uint32_t width, uint32_t height) {
    if (!CreateConversionImage(width, height, input_color_)) {
      return false;
    }
    if (!CreateConversionImage(width, height, output_color_)) {
      ShutdownConversionImages();
      return false;
    }
    return true;
  }

  void ShutdownConversionImages() {
    const VkDevice vk_device = device_.device();
    const auto& functions = device_.functions();
    for (ConversionImage* image : {&input_color_, &output_color_}) {
      if (image->view != VK_NULL_HANDLE) {
        functions.vkDestroyImageView(vk_device, image->view, nullptr);
      }
      if (image->image != VK_NULL_HANDLE) {
        functions.vkDestroyImage(vk_device, image->image, nullptr);
      }
      if (image->memory != VK_NULL_HANDLE) {
        functions.vkFreeMemory(vk_device, image->memory, nullptr);
      }
      *image = {};
    }
  }

  void QueryCapabilities() {
    const auto physical_device = device_.physical_device();
    const auto& ifn = device_.vulkan_instance()->functions();

    VkPhysicalDeviceProperties properties{};
    ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_2) {
      capability_reason_ = "Vulkan 1.2 is required";
      SetStatus(std::string("Frame generation: FSR unavailable (") + capability_reason_ + ")");
      return;
    }

    const auto& device_properties = device_.properties();
    if (!device_properties.shaderStorageImageReadWithoutFormat ||
        !device_properties.shaderStorageImageWriteWithoutFormat) {
      capability_reason_ =
          "shaderStorageImageReadWithoutFormat and "
          "shaderStorageImageWriteWithoutFormat are not enabled";
      SetStatus(std::string("Frame generation: FSR unavailable (") + capability_reason_ + ")");
      return;
    }

    if (!device_.extensions().ext_KHR_compute_shader_derivatives) {
      capability_reason_ = "VK_KHR_compute_shader_derivatives is not enabled";
      SetStatus(std::string("Frame generation: FSR unavailable (") + capability_reason_ + ")");
      return;
    }

    if (!device_properties.computeDerivativeGroupLinear) {
      capability_reason_ = "computeDerivativeGroupLinear is not enabled";
      SetStatus(std::string("Frame generation: FSR unavailable (") + capability_reason_ + ")");
      return;
    }

    // Context and pipeline creation in Prepare is the definitive capability
    // check; this gate only avoids attempting a known-unsupported provider.
    capability_available_ = true;
    capability_reason_ = "provider context has not been created";
    SetStatus("Frame generation: FSR capability present, waiting for Prepare");
  }

  void ShutdownContext() {
    if (context_) {
      ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context_);
      context_ = nullptr;
    }
  }

  const VulkanDevice& device_;
  FfxVkFsr3_3_1_6FrameGenerationContext* context_ = nullptr;
  ConversionImage input_color_;
  ConversionImage output_color_;
  bool capability_available_ = false;
  const char* capability_reason_ = "capabilities have not been queried";
  bool prepared_ = false;
  bool needs_reset_ = true;
  bool has_recorded_ = false;
  bool has_unretired_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  VkFormat color_format_ = VK_FORMAT_UNDEFINED;
  uint64_t highest_recorded_frame_id_ = 0;
  uint64_t highest_retired_frame_id_ = 0;
};

VulkanFsrFrameGeneration::VulkanFsrFrameGeneration(const VulkanDevice& device)
    : impl_(std::make_unique<Impl>(device)) {}

VulkanFsrFrameGeneration::~VulkanFsrFrameGeneration() = default;

bool VulkanFsrFrameGeneration::Prepare(uint32_t width, uint32_t height, VkFormat format) {
  return impl_->Prepare(width, height, format);
}

bool VulkanFsrFrameGeneration::Record(VkCommandBuffer command_buffer, const Input& input,
                                      const Output& output, uint64_t frame_id, bool reset) {
  return impl_->Record(command_buffer, input, output, frame_id, reset);
}

bool VulkanFsrFrameGeneration::Record(VkCommandBuffer command_buffer, const Input& input,
                                      const Output& output) {
  return impl_->Record(command_buffer, input, output, input.render_frame_index, input.reset);
}

void VulkanFsrFrameGeneration::Retire(uint64_t completed_frame_id) {
  impl_->Retire(completed_frame_id);
}

bool VulkanFsrFrameGeneration::Available() const {
  return impl_->Available();
}

void VulkanFsrFrameGeneration::Shutdown() {
  impl_->Shutdown();
}

std::string Fm4FsrFrameGenerationStatus() {
  std::lock_guard lock(g_status_mutex);
  return g_status;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
