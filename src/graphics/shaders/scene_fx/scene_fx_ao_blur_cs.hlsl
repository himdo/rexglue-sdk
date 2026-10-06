// Separable bilateral denoise of an effect along fx_blur_direction, at its
// resolution (fx_effect_scale; FX_RGBA: four channels, for the global
// illumination and the volumetric lighting; otherwise the ambient occlusion).
// Neighbors are weighted by how well their depth matches the center's surface
// plane, so slanted surfaces are still smoothed but nothing bleeds across
// depth edges.

#include "scene_fx_common.hlsli"

#ifdef FX_RGBA
#define FX_VALUE float4
#define FX_DEST_FORMAT FX_FORMAT_RGBA16F
#else
#define FX_VALUE float
#define FX_DEST_FORMAT FX_FORMAT_R16F
#endif

FX_BINDING(0) Texture2D<FX_VALUE> fx_source : register(t0);
// View distance at the effect's resolution (MIP 0).
FX_BINDING(1) Texture2D<float> fx_effect_depth : register(t1);
FX_BINDING(2) FX_DEST_FORMAT RWTexture2D<FX_VALUE> fx_dest : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 size = FxEffectSize(fx_effect_scale);
  if (any(int2(id.xy) >= size)) {
    return;
  }
  int2 center = int2(id.xy);
  int2 axis = int2(fx_blur_direction);
  float center_depth = fx_effect_depth.Load(int3(center, 0));
#ifndef FX_RGBA
  if (center_depth >= fx_sky_distance) {
    fx_dest[center] = 1.0;
    return;
  }
#endif
  // The depth slope along the axis, from the flatter side.
  float before = fx_effect_depth.Load(int3(clamp(center - axis, int2(0, 0), size - 1), 0));
  float after = fx_effect_depth.Load(int3(clamp(center + axis, int2(0, 0), size - 1), 0));
  float slope = abs(after - center_depth) < abs(center_depth - before) ? after - center_depth
                                                                         : center_depth - before;
  float tolerance = 0.02 * center_depth + 0.01;
  // The same footprint in scene pixels at either resolution.
  int radius = fx_effect_scale == 1u ? 8 : 4;
  float falloff = fx_effect_scale == 1u ? 0.02 : 0.08;
  FX_VALUE sum = 0.0;
  float weight_sum = 0.0;
  for (int i = -radius; i <= radius; ++i) {
    int2 sample_texel = clamp(center + axis * i, int2(0, 0), size - 1);
    float depth = fx_effect_depth.Load(int3(sample_texel, 0));
    float plane_error = abs(depth - (center_depth + slope * float(i)));
    float weight = exp(-falloff * float(i * i)) * saturate(1.0 - plane_error / tolerance);
    sum += fx_source.Load(int3(sample_texel, 0)) * weight;
    weight_sum += weight;
  }
  // The center always matches its own plane, so weight_sum > 0.
  fx_dest[center] = sum / weight_sum;
}
