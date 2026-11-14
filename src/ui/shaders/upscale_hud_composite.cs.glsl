#version 460

// UP-1.2: puts the game's HUD back over a temporal upscale of the HUD-less
// guest output. The HUD-less image's alpha is the HUD's coverage (the swap's
// HUD mask, already grown by a pixel for antialiased edges); where it is set,
// the output takes the guest output with the HUD, upscaled bilinearly, so the
// provider's history never holds the HUD.

layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform UpscaleHudCompositeConstants {
  uvec2 output_size;
  vec2 output_size_inv;
  // The share of the source images the upscaler read (its input size over
  // the image size).
  vec2 source_uv_scale;
  // fh1_native_hud: native_hud_image is the HUD drawn at the presentation's
  // resolution (premultiplied, coverage in alpha), which goes over the
  // upscale; the HUD-less image's alpha then marks the mirror glass alone,
  // put back from the frame as before.
  uint native_hud;
} constants;

layout(set = 0, binding = 0) uniform sampler2D frame;
layout(set = 0, binding = 1) uniform sampler2D hudless;
layout(set = 0, binding = 2, rgb10_a2) uniform image2D upscaled;
layout(set = 0, binding = 3) uniform sampler2D native_hud_image;

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, constants.output_size))) {
    return;
  }
  if (constants.native_hud != 0u) {
    vec2 native_uv = (vec2(pixel) + 0.5) * constants.output_size_inv;
    vec4 native_hud = textureLod(native_hud_image, native_uv, 0.0);
    float glass = textureLod(hudless, native_uv * constants.source_uv_scale, 0.0).a;
    if (native_hud.a <= 0.0 && glass <= 0.0) {
      return;
    }
    vec3 scene = imageLoad(upscaled, ivec2(pixel)).rgb;
    if (glass > 0.0) {
      scene = mix(scene, textureLod(frame, native_uv * constants.source_uv_scale, 0.0).rgb, glass);
    }
    imageStore(upscaled, ivec2(pixel),
               vec4(scene * (1.0 - native_hud.a) + native_hud.rgb, 1.0));
    return;
  }
  vec2 uv = (vec2(pixel) + 0.5) * constants.output_size_inv * constants.source_uv_scale;
  float hud = textureLod(hudless, uv, 0.0).a;
  if (hud <= 0.0) {
    return;
  }
  vec4 color = imageLoad(upscaled, ivec2(pixel));
  color.rgb = mix(color.rgb, textureLod(frame, uv, 0.0).rgb, hud);
  imageStore(upscaled, ivec2(pixel), vec4(color.rgb, 1.0));
}
