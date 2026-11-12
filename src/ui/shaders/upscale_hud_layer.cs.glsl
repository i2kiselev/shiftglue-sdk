#version 460
// Frame generation's HUD layer: in place of upscale_hud_composite.cs.glsl
// while frame generation runs, the game's HUD goes into a layer of its own,
// with its coverage as straight alpha, which frame generation's present
// callback (frame_generation_hud_compose.cs.glsl) puts over every presented
// frame, rendered and generated, so the HUD is never interpolated. Same inputs
// as upscale_hud_composite: the HUD-less image's alpha is the HUD's coverage,
// and the guest output with the HUD gives its colour, both upscaled
// bilinearly. Where both layers meet the result is the composite's,
// mix(scene, hud, coverage).
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;
layout(push_constant) uniform UpscaleHudLayerConstants {
  uvec2 output_size;
  vec2 output_size_inv;
  // The share of the source images the upscaler read (its input size over
  // the image size).
  vec2 source_uv_scale;
  // fh1_native_hud: native_hud_image is the HUD drawn at the presentation's
  // resolution (premultiplied), which becomes the layer over the mirror
  // glass the HUD-less image's alpha then marks alone.
  uint native_hud;
} constants;
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(set = 0, binding = 1) uniform sampler2D hudless;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D hud_layer;
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
    // The HUD over the glass, premultiplied, then straight for the layer.
    vec3 under = glass > 0.0
                     ? textureLod(frame, native_uv * constants.source_uv_scale, 0.0).rgb * glass
                     : vec3(0.0);
    vec3 premultiplied = native_hud.rgb + under * (1.0 - native_hud.a);
    float coverage = native_hud.a + glass * (1.0 - native_hud.a);
    vec3 straight = coverage > 0.0 ? min(premultiplied / coverage, vec3(1.0)) : vec3(0.0);
    imageStore(hud_layer, ivec2(pixel), vec4(straight, coverage));
    return;
  }
  vec2 uv = (vec2(pixel) + 0.5) * constants.output_size_inv * constants.source_uv_scale;
  float hud = textureLod(hudless, uv, 0.0).a;
  vec3 color = vec3(0.0);
  if (hud > 0.0) {
    color = textureLod(frame, uv, 0.0).rgb;
  }
  imageStore(hud_layer, ivec2(pixel), vec4(color, hud));
}
