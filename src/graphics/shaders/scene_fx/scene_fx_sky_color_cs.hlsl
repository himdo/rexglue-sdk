// Estimates the sky's HDR color from the guest's own rendering of it (the
// pixels at the far plane within fx_pass_rect of an HDR render target), so the
// volumetric lighting matches the game's time of day, weather and exposure.
// Smoothed over frames; kept as is when no sky is visible. One 16x16 group.

#include "scene_fx_common.hlsli"

#ifdef FX_MSAA
FX_BINDING(0) Texture2DMS<float4> fx_color : register(t0);
#else
FX_BINDING(0) Texture2D<float4> fx_color : register(t0);
#endif
FX_BINDING(1) Texture2D<float> fx_depth : register(t1);           // full resolution, linear
FX_BINDING(2) Texture2D<float4> fx_previous_sky : register(t2);   // 1x1, a = valid
FX_BINDING(3) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_sky : register(u0);          // 1x1

groupshared float4 fx_sky_sums[256];

[numthreads(16, 16, 1)]
void main(uint3 local : SV_GroupThreadID, uint index : SV_GroupIndex) {
  // A 32x32 grid of samples over the rectangle, 4 per thread.
  float4 sum = 0.0;
  float2 rect_size = float2(fx_pass_rect.zw - fx_pass_rect.xy);
  [unroll] for (uint i = 0; i < 4; ++i) {
    float2 grid = float2(local.xy * 2 + uint2(i & 1, i >> 1)) + 0.5;
    int2 pixel = int2(fx_pass_rect.xy) + int2(grid / 32.0 * rect_size);
    int2 screen = clamp(pixel + fx_screen_offset, int2(0, 0), int2(fx_scene_size) - 1);
    if (fx_depth.Load(int3(screen, 0)) >= fx_sky_distance) {
#ifdef FX_MSAA
      float3 color = fx_color.Load(pixel, 0).rgb;
#else
      float3 color = fx_color.Load(int3(pixel, 0)).rgb;
#endif
      // Ignore NaN / infinity from the guest's float formats.
      if (all(color == color) && all(color < 65504.0)) {
        sum += float4(max(color, 0.0), 1.0);
      }
    }
  }
  fx_sky_sums[index] = sum;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint stride = 128; stride > 0; stride >>= 1) {
    if (index < stride) {
      fx_sky_sums[index] += fx_sky_sums[index + stride];
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (index == 0) {
    float4 previous = fx_previous_sky.Load(int3(0, 0, 0));
    float4 total = fx_sky_sums[0];
    float4 sky = previous;
    // At least a few percent of the samples, so a sliver doesn't dominate.
    if (total.w >= 40.0) {
      float3 average = total.rgb / total.w;
      sky = float4(previous.a > 0.0 ? lerp(previous.rgb, average, 0.08) : average, 1.0);
    }
    fx_sky[uint2(0, 0)] = sky;
  }
}
