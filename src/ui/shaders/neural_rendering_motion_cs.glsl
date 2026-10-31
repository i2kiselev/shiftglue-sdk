#version 450

layout(set = 0, binding = 0) uniform sampler2D source_motion;
layout(set = 0, binding = 2, rg16f) uniform image2D destination_motion;

layout(push_constant) uniform Parameters {
  uvec2 size;
  vec2 scale;
} parameters;

layout(local_size_x = 8, local_size_y = 8) in;

void main() {
  const uvec2 position = gl_GlobalInvocationID.xy;
  if (position.x >= parameters.size.x || position.y >= parameters.size.y) {
    return;
  }
  const vec2 uv_motion =
      texelFetch(source_motion, ivec2(position), 0).xy;
  imageStore(destination_motion, ivec2(position),
             vec4(uv_motion * parameters.scale, 0.0, 0.0));
}
