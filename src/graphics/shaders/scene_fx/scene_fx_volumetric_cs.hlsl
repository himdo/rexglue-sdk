// Volumetric lighting (light shafts) at full or half resolution
// (fx_effect_scale): ray-marches each pixel's view ray through the air,
// accumulating the sunlight it scatters toward the camera wherever the guest's
// own sun shadow cascades say the sun reaches - so shafts form through
// buildings, trees and windows. Shadows are bilinearly filtered (PCF) for
// smooth shaft edges, and the per-pixel jitter changes every frame for the
// temporal accumulation. The light is relative to the guest's sky, following
// its time of day, weather and exposure.
// Output: rgb = scattered sunlight reaching the camera.

#include "scene_fx_sun.hlsli"

// View distance at the effect's resolution (MIP 0).
FX_BINDING(0) Texture2D<float> fx_effect_depth : register(t0);
FX_BINDING(1) Texture2D<float> fx_shadow0 : register(t1);
FX_BINDING(2) Texture2D<float> fx_shadow1 : register(t2);
FX_BINDING(3) Texture2D<float4> fx_sky : register(t3);  // 1x1
FX_BINDING(4) SamplerState fx_clamp : register(s0);
FX_SUN_CONSTANTS(5)
FX_BINDING(6) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_volumetric : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(int2(id.xy) >= FxEffectSize(fx_effect_scale))) {
    return;
  }
  int2 texel = int2(id.xy);
  float view_distance = fx_effect_depth.Load(int3(texel, 0));
  float3 direction =
      normalize(FxViewPosition(FxEffectScreen(texel, fx_effect_scale), 1.0));
  // Distance along the ray to the surface (the sky is beyond the maximum).
  float surface_distance =
      view_distance >= fx_sky_distance ? 1.0e30 : view_distance / direction.z;
  float march_distance = min(surface_distance, fx_sun_volumetric.x);
  uint step_count = max(uint(fx_sun_volumetric.y), 1u);
  // Varies every frame - the temporal accumulation averages it out.
  float jitter = FxNoise(float2(texel) + 5.588238 * float(fx_frame & 63));

  // Henyey-Greenstein, scaled so isotropic scattering is 1.
  float g = fx_sun_direction.w;
  float cos_theta = dot(direction, fx_sun_direction.xyz);
  float phase = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cos_theta, 1.0e-4), 1.5);
  float3 sun_light =
      fx_sky.Load(int3(0, 0, 0)).rgb * fx_sun_tint.rgb * (fx_sun_volumetric.z * phase);

  float transmittance = 1.0;
  float3 scattered = 0.0;
  float previous_distance = 0.0;
  for (uint i = 0; i < step_count; ++i) {
    // Quadratically spaced: denser near the camera, where shafts are sharpest.
    float s = (float(i) + jitter) / float(step_count);
    float distance = s * s * march_distance;
    float step_length = distance - previous_distance;
    previous_distance = distance;
    float3 position = direction * distance;
    float density = fx_sun_volumetric.w;
    if (fx_sun_tint.w > 0.0) {
      float height = dot(fx_sun_height, float4(position, 1.0));
      density += fx_sun_fog.w + fx_sun_fog.x * exp(min(-(height - fx_sun_fog.y) * fx_sun_fog.z,
                                                       20.0));
    }
    float lit;
    FX_SUN_SHADOW(fx_shadow0, fx_shadow1, fx_clamp, position, lit)
    float step_transmittance = exp(-density * step_length);
    // Energy-conserving: what this segment scatters toward the camera.
    scattered += transmittance * (1.0 - step_transmittance) * lit * sun_light;
    transmittance *= step_transmittance;
  }
  fx_volumetric[texel] = float4(scattered, 1.0);
}
