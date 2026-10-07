// Temporal accumulation of an effect at its resolution (fx_effect_scale; the
// volumetric lighting): blends this frame's jittered result into the history
// reprojected from the previous frame. The history is clamped to the range of
// this frame's 3x3 neighborhood, so moving objects and disocclusions don't
// leave trails.

#include "scene_fx_common.hlsli"

FX_BINDING(0) Texture2D<float4> fx_current : register(t0);
FX_BINDING(1) Texture2D<float4> fx_history : register(t1);
// View distance at the effect's resolution (MIP 0).
FX_BINDING(2) Texture2D<float> fx_effect_depth : register(t2);
FX_BINDING(3) SamplerState fx_linear_clamp : register(s0);
FX_EXTRA_CONSTANTS_BLOCK(4) {
  // View space position this frame to the previous frame's clip space.
  float4 fx_temporal_reproject[4];
  // x = weight of this frame (1 = no history), y = max distance reprojected.
  float4 fx_temporal_params;
};
FX_BINDING(5) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_accumulated : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 size = FxEffectSize(fx_effect_scale);
  if (any(int2(id.xy) >= size)) {
    return;
  }
  int2 texel = int2(id.xy);
  float4 current = fx_current.Load(int3(texel, 0));
  float4 neighborhood_min = current;
  float4 neighborhood_max = current;
  [unroll] for (int i = 0; i < 9; ++i) {
    if (i == 4) {
      continue;
    }
    int2 neighbor = clamp(texel + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), size - 1);
    float4 value = fx_current.Load(int3(neighbor, 0));
    neighborhood_min = min(neighborhood_min, value);
    neighborhood_max = max(neighborhood_max, value);
  }

  float4 result = current;
  if (fx_temporal_params.x < 1.0) {
    // Where the surface (or the far end of the effect) was last frame.
    float distance = min(fx_effect_depth.Load(int3(texel, 0)), fx_temporal_params.y);
    float4 p = float4(FxViewPosition(FxEffectScreen(texel, fx_effect_scale), distance), 1.0);
    float4 clip = float4(dot(fx_temporal_reproject[0], p), dot(fx_temporal_reproject[1], p),
                         dot(fx_temporal_reproject[2], p), dot(fx_temporal_reproject[3], p));
    if (clip.w > 1.0e-4) {
      float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
      if (all(uv >= 0.0) && all(uv <= 1.0)) {
        // The history texture may be larger than the effect.
        float2 history_size;
        fx_history.GetDimensions(history_size.x, history_size.y);
        float4 history =
            fx_history.SampleLevel(fx_linear_clamp, uv * float2(size) / history_size, 0.0);
        history = clamp(history, neighborhood_min, neighborhood_max);
        result = lerp(history, current, fx_temporal_params.x);
      }
    }
  }
  fx_accumulated[texel] = result;
}
