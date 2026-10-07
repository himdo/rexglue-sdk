// Contact shadows at full or half resolution (fx_effect_scale): a short
// screen-space ray toward the sun from each surface catches the fine shadows
// the guest's shadow maps are too coarse for - under feet, props and grass,
// in crevices. Only where the shadow maps say the sun reaches, so nothing is
// shadowed twice. Output 1 = lit.

#include "scene_fx_sun.hlsli"

// View distance at the effect's resolution (MIP 0).
FX_BINDING(0) Texture2D<float> fx_effect_depth : register(t0);
// Full resolution, for the ray tests.
FX_BINDING(1) Texture2D<float> fx_depth : register(t1);
FX_BINDING(2) Texture2D<float> fx_shadow0 : register(t2);
FX_BINDING(3) Texture2D<float> fx_shadow1 : register(t3);
FX_BINDING(4) SamplerState fx_clamp : register(s0);
FX_SUN_CONSTANTS(5)
FX_BINDING(6) FX_FORMAT_R16F RWTexture2D<float> fx_contact : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(int2(id.xy) >= FxEffectSize(fx_effect_scale))) {
    return;
  }
  int2 texel = int2(id.xy);
  float distance = fx_effect_depth.Load(int3(texel, 0));
  if (distance >= fx_sky_distance) {
    fx_contact[texel] = 1.0;
    return;
  }
  float3 position = FxViewPosition(FxEffectScreen(texel, fx_effect_scale), distance);
  // Already in the guest's shadow: nothing to add.
  float lit;
  FX_SUN_SHADOW(fx_shadow0, fx_shadow1, fx_clamp, position, lit)
  if (lit <= 0.01) {
    fx_contact[texel] = 1.0;
    return;
  }

  float ray_length = fx_sun_contact.x;
  float thickness = fx_sun_contact.y;
  uint step_count = max(uint(fx_sun_contact.z), 1u);
  float jitter = FxNoise(float2(texel));
  // Not the surface itself: depth precision and the slope it's seen at.
  float bias = 0.002 * distance + 0.01;
  float occlusion = 0.0;
  for (uint i = 0; i < step_count; ++i) {
    float t = (float(i) + jitter) / float(step_count);
    float3 sample_position = position + fx_sun_direction.xyz * (ray_length * t);
    if (sample_position.z <= 0.05) {
      break;
    }
    float2 ndc = sample_position.xy /
                 (sample_position.z * float2(fx_inv_p00, fx_inv_p11));
    float2 screen = (ndc * float2(0.5, -0.5) + 0.5) * float2(fx_scene_size);
    if (any(screen < 0.0) || any(screen >= float2(fx_scene_size))) {
      break;
    }
    float scene_distance = fx_depth.Load(int3(int2(screen), 0));
    float behind = sample_position.z - scene_distance;
    if (behind > bias && behind < thickness) {
      // Softer the further along the ray the occluder is.
      occlusion = 1.0 - t * t;
      break;
    }
  }
  fx_contact[texel] = 1.0 - occlusion * lit;
}
