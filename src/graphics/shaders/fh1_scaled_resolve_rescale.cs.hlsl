// Resamples resolved data from one draw resolution scale's layout into
// another's (nearest host texel), so a scale switch keeps what the title
// rendered before it. Vulkan only, in the texture cache's load pipeline layout:
// set 0 is the destination, set 1 the source, both bound at the run's start.
//
// The texture cache's scaled resolve layout: each 16 bytes of the guest
// texture become scale x scale groups of 16 bytes, each holding one host row
// of the guest group's texels at scale, stored column-major by group. A scale
// of 1 is the guest layout itself. The mapping depends on the texel size only,
// so it is done byte by byte.
#include "fh1_push_constants.hlsli"

FH1_PUSH_CONSTANTS cbuffer Rescale FH1_CONSTANTS_REGISTER {
  uint guest_length;  // bytes, unscaled, a multiple of 16
  uint source_scale;
  uint dest_scale;
  uint bpb_log2;
};

[[vk::binding(0, 1)]] ByteAddressBuffer source;
[[vk::binding(0, 0)]] RWByteAddressBuffer dest;

uint SourceByte(uint dest_byte) {
  uint texels_per_group_log2 = 4u - bpb_log2;
  uint group = dest_byte / (16u * dest_scale * dest_scale);
  uint in_group = dest_byte % (16u * dest_scale * dest_scale);
  uint host_group = in_group >> 4u;
  uint host_x = (host_group / dest_scale << texels_per_group_log2) + ((in_group & 15u) >> bpb_log2);
  uint subpixel_y = host_group % dest_scale;
  uint texel_byte = in_group & ((1u << bpb_log2) - 1u);
  uint source_x =
      host_x / dest_scale * source_scale + host_x % dest_scale * source_scale / dest_scale;
  uint source_y = subpixel_y * source_scale / dest_scale;
  uint source_group = (source_x >> texels_per_group_log2) * source_scale + source_y;
  return group * 16u * source_scale * source_scale + (source_group << 4u) +
         ((source_x & ((1u << texels_per_group_log2) - 1u)) << bpb_log2) + texel_byte;
}

[numthreads(256, 1, 1)]
void main(uint3 thread_id : SV_DispatchThreadID) {
  uint dest_byte = thread_id.x << 2u;
  if (dest_byte >= guest_length * dest_scale * dest_scale) {
    return;
  }
  uint word = 0u;
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    uint byte_address = SourceByte(dest_byte + i);
    uint value = (source.Load(byte_address & ~3u) >> ((byte_address & 3u) << 3u)) & 0xFFu;
    word |= value << (i << 3u);
  }
  dest.Store(dest_byte, word);
}
