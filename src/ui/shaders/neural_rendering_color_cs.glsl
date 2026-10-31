#version 450

layout(set = 0, binding = 0) uniform sampler2D source_color;
layout(set = 0, binding = 2, rgba16f) uniform image2D destination_color;

layout(push_constant) uniform Parameters {
  uvec2 size;
} parameters;

layout(local_size_x = 8, local_size_y = 8) in;

void main() {
  const uvec2 position = gl_GlobalInvocationID.xy;
  if (position.x >= parameters.size.x || position.y >= parameters.size.y) {
    return;
  }
  imageStore(destination_color, ivec2(position),
             texelFetch(source_color, ivec2(position), 0));
}
