/**
 * @file        graphics/vulkan/fh1_hdr_gain.cpp
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
// present_hdr_native: FM4's HDR gain map. FM4 tonemaps its float scene into the 10-bit front
// buffer with a Hable filmic curve that leaves little above SDR white
// (fsr4-patch/hdr/POST_CHAIN.md). At that draw this pass reads the same inputs - the scene, the
// bloom, the eye adaptation's average luminance and the curve's constants - and writes, per
// pixel, how much brighter than the game's SDR picture the pixel is on an HDR display: 1 up
// to a knee, then the scene's own brightness rolled off towards the display's peak. The
// presenter multiplies the decoded SDR picture by it (hdr_output.ps.glsl), so the game's
// grading, saturation, upscaling and HUD stay as they are.

#include <rex/graphics/vulkan/fh1_native_executor.h>

#include <algorithm>
#include <string>
#include <vector>

#include <rex/graphics/vulkan/command_processor.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

namespace rex::graphics::vulkan {

namespace {

const char* const kHdrGainSource = R"(#version 450
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform HdrGainConstants {
  // The tonemap vertex shader's c2 (luminance keys), c3 (their log2 exposures), c4 (white
  // point W, Hable A, B, C) and c5 (D, E, F, log2 exposure offset).
  vec4 keys;
  vec4 key_exposures;
  vec4 wabc;
  vec4 def_offset;
  // Output UV to where the tonemap samples the scene and the bloom: scale xy, offset zw.
  vec4 scene_map;
  vec4 bloom_map;
  // The fetches' exponent scales (2^exp_adjust) of the scene and the bloom, the display's
  // decoding gamma, and the HDR peak over SDR white.
  float scene_scale;
  float bloom_scale;
  float gamma;
  float peak;
  uvec2 size;
  // The game's SDR brightness (after its curve, 0 to 1) where the expansion starts.
  float knee;
} c;
// The texture cache's views of 2D textures are 2D arrays.
layout(set = 0, binding = 0) uniform texture2DArray scene;
layout(set = 0, binding = 1) uniform texture2DArray bloom;
layout(set = 0, binding = 2) uniform texture2DArray luminance;
layout(set = 0, binding = 3, r16f) writeonly uniform image2D gain;

float Hable(float x) {
  float a = c.wabc.y, b = c.wabc.z, cc = c.wabc.w;
  float d = c.def_offset.x, e = c.def_offset.y, f = c.def_offset.z;
  return (x * (a * x + cc * b) + d * e) / (x * (a * x + b) + d * f) - e / f;
}

// The game's curve, normalised to its white point: SDR brightness 0 to 1.
float Curve(float x) { return Hable(x) / Hable(c.wabc.x); }

vec3 Bilinear(texture2DArray t, vec2 uv) {
  ivec2 size = textureSize(t, 0).xy;
  vec2 p = uv * vec2(size) - 0.5;
  ivec2 i = ivec2(floor(p));
  vec2 f = p - vec2(i);
  ivec2 last = size - 1;
  vec3 a = texelFetch(t, ivec3(clamp(i, ivec2(0), last), 0), 0).rgb;
  vec3 b = texelFetch(t, ivec3(clamp(i + ivec2(1, 0), ivec2(0), last), 0), 0).rgb;
  vec3 d = texelFetch(t, ivec3(clamp(i + ivec2(0, 1), ivec2(0), last), 0), 0).rgb;
  vec3 e = texelFetch(t, ivec3(clamp(i + ivec2(1, 1), ivec2(0), last), 0), 0).rgb;
  return mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
}

// The tonemap vertex shader's eye adaptation: the average luminance's place between two of
// the four keys gives the log2 exposure between theirs.
float Exposure() {
  float l = texelFetch(luminance, ivec3(textureSize(luminance, 0).xy / 2, 0), 0).r;
  float low_key, high_key, low_exposure, high_exposure;
  if (l < c.keys.y) {
    low_key = c.keys.x; high_key = c.keys.y;
    low_exposure = c.key_exposures.x; high_exposure = c.key_exposures.y;
  } else if (l >= c.keys.z) {
    low_key = c.keys.z; high_key = c.keys.w;
    low_exposure = c.key_exposures.z; high_exposure = c.key_exposures.w;
  } else {
    low_key = c.keys.y; high_key = c.keys.z;
    low_exposure = c.key_exposures.y; high_exposure = c.key_exposures.z;
  }
  float t = clamp((l - low_key) / (high_key - low_key), 0.0, 1.0);
  return exp2(low_exposure + c.def_offset.w + t * (high_exposure - low_exposure));
}

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, c.size))) {
    return;
  }
  vec2 uv = (vec2(pixel) + 0.5) / vec2(c.size);
  vec3 s = Bilinear(scene, uv * c.scene_map.xy + c.scene_map.zw) * c.scene_scale;
  vec3 b = Bilinear(bloom, uv * c.bloom_map.xy + c.bloom_map.zw) * c.bloom_scale;
  vec3 x = (s * s + b * b) * Exposure();
  // Mostly the largest channel, as present_hdr's expansion: coloured lights expand too.
  float y = mix(dot(x, vec3(0.2126, 0.7152, 0.0722)), max(x.r, max(x.g, x.b)), 0.75);
  float sdr = Curve(y);
  float result = 1.0;
  if (sdr > c.knee && c.peak > 1.0) {
    // Where the curve reaches the knee, and its slope there.
    float low = 0.0, high = max(y, 1.0e-4);
    for (int i = 0; i < 24; ++i) {
      float middle = 0.5 * (low + high);
      if (Curve(middle) < c.knee) low = middle; else high = middle;
    }
    float knee_x = 0.5 * (low + high);
    float slope = (Curve(knee_x * 1.01) - Curve(knee_x)) / (knee_x * 0.01);
    // Straight on from the knee with that slope, rolled off towards the peak.
    float linear = slope * (y - knee_x);
    float headroom = c.peak - c.knee;
    float hdr = c.knee + headroom * (1.0 - exp(-linear / headroom));
    // In the display's decoding: the SDR picture is the curve's square root.
    result = pow(max(hdr, sdr) / max(sdr, 1.0e-4), 0.5 * c.gamma);
  }
  imageStore(gain, ivec2(pixel), vec4(result, 0.0, 0.0, 0.0));
}
)";

}  // namespace

bool Fh1NativeExecutor::EnsureHdrGainPipeline() {
  if (hdr_gain_pipeline_ != VK_NULL_HANDLE) return true;
  if (hdr_gain_failed_) return false;
  hdr_gain_failed_ = true;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkDescriptorSetLayoutBinding bindings[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType =
        i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 4;
  set_layout_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &hdr_gain_set_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  VkPushConstantRange push_constant_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                             sizeof(HdrGainConstants)};
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &hdr_gain_set_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                 &hdr_gain_pipeline_layout_) != VK_SUCCESS) {
    return false;
  }
  std::vector<uint32_t> spirv;
  std::string error;
  if (!command_processor_.CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, kHdrGainSource, spirv,
                                             error)) {
    REXGPU_WARN("Failed to compile the HDR gain shader: {}", error);
    return false;
  }
  hdr_gain_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, hdr_gain_pipeline_layout_, spirv.data(), spirv.size() * sizeof(uint32_t));
  if (hdr_gain_pipeline_ == VK_NULL_HANDLE) return false;
  hdr_gain_failed_ = false;
  return true;
}

void Fh1NativeExecutor::DestroyHdrGainImage() {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (hdr_gain_.view) dfn.vkDestroyImageView(device, hdr_gain_.view, nullptr);
  if (hdr_gain_.image) dfn.vkDestroyImage(device, hdr_gain_.image, nullptr);
  if (hdr_gain_.memory) dfn.vkFreeMemory(device, hdr_gain_.memory, nullptr);
  hdr_gain_ = Surface();
  hdr_gain_ready_ = false;
}

void Fh1NativeExecutor::CaptureHdrGain(const HdrGainInputs& inputs) {
  if (!initialized_ || !inputs.scene || !inputs.bloom || !inputs.luminance ||
      !inputs.constants.size[0] || !inputs.constants.size[1] || !EnsureHdrGainPipeline()) {
    return;
  }
  const uint32_t width = inputs.constants.size[0];
  const uint32_t height = inputs.constants.size[1];
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  command_processor_.EndRenderPass();
  if (hdr_gain_.image != VK_NULL_HANDLE &&
      (hdr_gain_.width != width || hdr_gain_.height != height)) {
    // Earlier frames may still read it; only on a resolution change.
    command_processor_.Fh1AwaitAllQueueOperations();
    DestroyHdrGainImage();
  }
  if (hdr_gain_.image == VK_NULL_HANDLE) {
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R16_SFLOAT;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            hdr_gain_.image, hdr_gain_.memory)) {
      hdr_gain_ = Surface();
      Skip("hdr_gain_create");
      return;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = hdr_gain_.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R16_SFLOAT;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &hdr_gain_.view) != VK_SUCCESS) {
      DestroyHdrGainImage();
      Skip("hdr_gain_create");
      return;
    }
    hdr_gain_.format = VK_FORMAT_R16_SFLOAT;
    hdr_gain_.width = width;
    hdr_gain_.height = height;
  }
  Transition(hdr_gain_, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
             VK_ACCESS_SHADER_WRITE_BIT);
  command_processor_.SubmitBarriers(true);
  const VkDescriptorSet set = AllocateDescriptorSet(hdr_gain_set_layout_);
  if (set == VK_NULL_HANDLE) {
    Skip("hdr_gain_descriptor");
    return;
  }
  // The game's textures are sampled by the tonemap draw just recorded, in this layout.
  VkDescriptorImageInfo images[4] = {
      {VK_NULL_HANDLE, inputs.scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, inputs.bloom, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, inputs.luminance, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, hdr_gain_.view, VK_IMAGE_LAYOUT_GENERAL},
  };
  VkWriteDescriptorSet writes[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType =
        i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  dfn.vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                         hdr_gain_pipeline_layout_, 0, 1, &set, 0, nullptr);
  command_buffer.CmdVkPushConstants(hdr_gain_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    sizeof(inputs.constants), &inputs.constants);
  command_processor_.BindExternalComputePipeline(hdr_gain_pipeline_);
  command_buffer.CmdVkDispatch((width + 7) / 8, (height + 7) / 8, 1);
  hdr_gain_taken_ = true;
  Count("hdr_gain");
}

VkImage Fh1NativeExecutor::TakeHdrGain(uint32_t& width_out, uint32_t& height_out) {
  const bool ready = hdr_gain_ready_;
  hdr_gain_ready_ = false;
  if (!ready || hdr_gain_.image == VK_NULL_HANDLE) return VK_NULL_HANDLE;
  Transition(hdr_gain_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_ACCESS_TRANSFER_READ_BIT);
  width_out = hdr_gain_.width;
  height_out = hdr_gain_.height;
  return hdr_gain_.image;
}

}  // namespace rex::graphics::vulkan
