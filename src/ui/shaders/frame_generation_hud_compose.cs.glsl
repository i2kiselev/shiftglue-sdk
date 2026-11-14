#version 460
// Frame generation's present callback: puts the HUD layer
// (upscale_hud_layer.cs.glsl, straight alpha) over a presented frame, rendered
// or generated, in a copy of it. The copy has the swapchain's format, written
// through an RGBA view: a BGRA one has red and blue swapped.
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform FrameGenerationHudComposeConstants {
  uvec2 size;
  uint swap_red_blue;
} constants;
layout(set = 0, binding = 0) uniform sampler2D hud_layer;
layout(set = 0, binding = 1, rgba8) uniform image2D frame;
void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, constants.size))) {
    return;
  }
  vec4 hud = texelFetch(hud_layer, ivec2(pixel), 0);
  if (hud.a <= 0.0) {
    return;
  }
  vec4 color = imageLoad(frame, ivec2(pixel));
  vec3 rgb = constants.swap_red_blue != 0u ? color.bgr : color.rgb;
  rgb = mix(rgb, hud.rgb, hud.a);
  imageStore(frame, ivec2(pixel), vec4(constants.swap_red_blue != 0u ? rgb.bgr : rgb, color.a));
}
