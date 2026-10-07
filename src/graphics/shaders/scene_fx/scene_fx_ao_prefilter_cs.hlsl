// Half-resolution view distance with 3 more MIP levels for the ambient
// occlusion, in one pass (after XeGTAO's depth prefilter). MIP 0 texel i is
// scene pixel 2i (the occlusion is computed there); coarser levels favor the
// farther depths, so thin foreground objects don't thicken at a distance.

#include "scene_fx_common.hlsli"

FX_BINDING(0) Texture2D<float> fx_depth : register(t0);
FX_BINDING(1) FX_FORMAT_R32F RWTexture2D<float> fx_mip0 : register(u0);
FX_BINDING(2) FX_FORMAT_R32F RWTexture2D<float> fx_mip1 : register(u1);
FX_BINDING(3) FX_FORMAT_R32F RWTexture2D<float> fx_mip2 : register(u2);
FX_BINDING(4) FX_FORMAT_R32F RWTexture2D<float> fx_mip3 : register(u3);

groupshared float fx_mip_cache[8][8];

float FxDepthMipFilter(float d0, float d1, float d2, float d3) {
  float max_depth = max(max(d0, d1), max(d2, d3));
  float effect_radius = 0.75 * fx_ao_radius;
  float falloff_range = 0.615 * effect_radius;
  float falloff_from = effect_radius * (1.0 - 0.615);
  float falloff_mul = -1.0 / falloff_range;
  float falloff_add = falloff_from / falloff_range + 1.0;
  float w0 = saturate((max_depth - d0) * falloff_mul + falloff_add);
  float w1 = saturate((max_depth - d1) * falloff_mul + falloff_add);
  float w2 = saturate((max_depth - d2) * falloff_mul + falloff_add);
  float w3 = saturate((max_depth - d3) * falloff_mul + falloff_add);
  return (w0 * d0 + w1 * d1 + w2 * d2 + w3 * d3) / (w0 + w1 + w2 + w3);
}

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID) {
  int2 half_size = int2((fx_scene_size + 1) >> 1);
  int2 texel = int2(group.xy * 8 + local.xy);
  float depth = fx_depth.Load(int3(min(texel * 2, int2(fx_scene_size) - 1), 0));
  if (all(texel < half_size)) {
    fx_mip0[texel] = depth;
  }
  fx_mip_cache[local.y][local.x] = depth;
  GroupMemoryBarrierWithGroupSync();

  // Each level from 2x2 of the previous one, the result kept at the top-left.
  [unroll] for (uint level = 1; level <= 3; ++level) {
    uint step = 1u << (level - 1);
    bool active = all((local.xy & ((step << 1) - 1)) == 0);
    float filtered = 0.0;
    if (active) {
      filtered = FxDepthMipFilter(
          fx_mip_cache[local.y][local.x], fx_mip_cache[local.y][local.x + step],
          fx_mip_cache[local.y + step][local.x], fx_mip_cache[local.y + step][local.x + step]);
      int2 level_texel = texel >> level;
      if (all(level_texel < max(half_size >> level, int2(1, 1)))) {
        if (level == 1) {
          fx_mip1[level_texel] = filtered;
        } else if (level == 2) {
          fx_mip2[level_texel] = filtered;
        } else {
          fx_mip3[level_texel] = filtered;
        }
      }
    }
    GroupMemoryBarrierWithGroupSync();
    if (active) {
      fx_mip_cache[local.y][local.x] = filtered;
    }
    GroupMemoryBarrierWithGroupSync();
  }
}
