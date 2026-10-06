// Composites the effects into the guest's HDR scene - those computed at half
// resolution upsampled with depth-aware (joint bilateral) weights - in up to
// two passes
// (fx_composite_mode):
// - Multiply: color *= (ambient occlusion + indirect light relative to the
//   surface's brightness) * fog transmittance.
// - Add: color += light the fog scatters toward the camera + volumetric
//   lighting (light shafts).
// - Debug: replaces the color with one effect.
// The fog is exponential height fog integrated analytically per pixel at full
// resolution (no ray marching), lit by the sky and with a glow toward the sun.

#include "scene_fx_common.hlsli"

// At fx_effect_scale.
FX_BINDING(0) Texture2D<float> fx_ao : register(t0);
FX_BINDING(1) Texture2D<float> fx_view_depth : register(t1);  // half resolution, MIP 0
FX_BINDING(2) Texture2D<float> fx_depth : register(t2);       // full resolution
// At fx_volumetric_scale.
FX_BINDING(3) Texture2D<float4> fx_volumetric : register(t3);
// At fx_effect_scale.
FX_BINDING(4) Texture2D<float4> fx_gi : register(t4);
// Half resolution, this frame's scene before the effects.
FX_BINDING(5) Texture2D<float4> fx_scene_color : register(t5);
FX_BINDING(6) Texture2D<float4> fx_sky : register(t6);  // 1x1
// At fx_effect_scale.
FX_BINDING(7) Texture2D<float> fx_contact_shadows : register(t7);
FX_BINDING(8) Texture2D<float4> fx_reflections : register(t8);
FX_EXTRA_CONSTANTS_BLOCK(9) {
  // xyz = view space direction to world height, w = the camera's height.
  float4 fx_fog_height;
  // Density: x = at height y (world), z = falloff with height, w = everywhere.
  float4 fx_fog_density;
  // rgb = skylight scattered by the fog relative to the sky's color, w =
  // distance the sky is at.
  float4 fx_fog_color;
  // xyz = toward the sun (view space), w = Henyey-Greenstein anisotropy.
  float4 fx_fog_sun;
  // rgb = sunlight scattered by the fog relative to the sky's color.
  float4 fx_fog_sun_color;
  // x = contact shadow strength, y = reflection intensity.
  float4 fx_effect_strengths;
};

// An effect at fx_effect_scale at this pixel: the pixel itself at full
// resolution, otherwise the joint bilateral upsampling.
#define FX_UPSAMPLE(texture, result)                                  \
  if (fx_effect_scale == 1u) {                                        \
    result = texture.Load(int3(pixel, 0));                            \
  } else {                                                            \
    result = 0.0;                                                     \
    [unroll] for (int i = 0; i < 4; ++i) {                            \
      result += texture.Load(int3(texels[i], 0)) * weights[i];        \
    }                                                                 \
    result *= inv_weight_sum;                                         \
  }

// Jimenez et al. 2016: visibility to occlusion including interreflections.
float FxMultiBounce(float visibility, float albedo) {
  float a = 2.0404 * albedo - 0.3324;
  float b = -4.7951 * albedo + 0.6417;
  float c = 2.7552 * albedo + 0.6903;
  return max(visibility, ((visibility * a + b) * visibility + c) * visibility);
}

// Optical depth from the camera along a unit view space direction:
// integral of everywhere + at_height * exp(-falloff * (height(s) - height_0)).
float FxFogOpticalDepth(float3 direction, float distance) {
  float rise = dot(fx_fog_height.xyz, direction);
  float at_camera =
      fx_fog_density.x * exp(min(-fx_fog_density.z * (fx_fog_height.w - fx_fog_density.y), 80.0));
  float k = fx_fog_density.z * rise;
  float integral = abs(k * distance) > 1.0e-4
                       ? (1.0 - exp(min(-k * distance, 80.0))) / k
                       : distance;
  return fx_fog_density.w * distance + at_camera * integral;
}

float4 main(float4 position : SV_Position) : SV_Target {
  float2 screen = position.xy * fx_output_scale + float2(fx_screen_offset);
  // Unchanged for either blend.
  float4 identity = fx_composite_mode == FX_COMPOSITE_ADD ? 0.0 : 1.0;
  if (fx_debug_mode == FX_DEBUG_SPLIT && screen.x >= 0.5 * float(fx_scene_size.x)) {
    return identity;
  }
  int2 pixel = clamp(int2(screen), int2(0, 0), int2(fx_scene_size) - 1);
  float depth = fx_depth.Load(int3(pixel, 0));
  bool is_sky = depth >= fx_sky_distance;

  // The four half-resolution texels around (texel i was computed at scene
  // pixel 2i), weighted bilinearly and by depth similarity.
  int2 half_size = int2((fx_scene_size + 1) >> 1);
  float2 half_position = float2(pixel) * 0.5;
  int2 base = int2(floor(half_position));
  float2 fraction = half_position - float2(base);
  float tolerance = 0.03 * depth + 0.01;
  int2 texels[4];
  float weights[4];
  float weight_sum = 0.0;
  float closest_error = 1.0e30;
  int closest = 0;
  [unroll] for (int i = 0; i < 4; ++i) {
    int2 corner = int2(i & 1, i >> 1);
    texels[i] = clamp(base + corner, int2(0, 0), half_size - 1);
    float error = abs(fx_view_depth.Load(int3(texels[i], 0)) - depth);
    float2 bilinear = lerp(1.0 - fraction, fraction, float2(corner));
    weights[i] = bilinear.x * bilinear.y * saturate(1.0 - error / tolerance);
    weight_sum += weights[i];
    if (error < closest_error) {
      closest_error = error;
      closest = i;
    }
  }
  // No similar depth around (a thin feature): the closest texel alone.
  if (weight_sum < 1.0e-3) {
    [unroll] for (int i = 0; i < 4; ++i) {
      weights[i] = i == closest ? 1.0 : 0.0;
    }
    weight_sum = 1.0;
  }
  float inv_weight_sum = 1.0 / weight_sum;
  // Effects at full resolution: the pixel itself.
  bool ao_full = fx_effect_scale == 1u;
  bool volumetric_full = fx_volumetric_scale == 1u;

  // Fog along the view ray to the surface (or the sky's distance).
  float fog_transmittance = 1.0;
  float3 fog_light = 0.0;
  if (fx_effect_flags & FX_EFFECT_FOG) {
    float3 direction = normalize(FxViewPosition(screen, 1.0));
    float distance = is_sky ? fx_fog_color.w : depth / direction.z;
    fog_transmittance = exp(-FxFogOpticalDepth(direction, distance));
    float g = fx_fog_sun.w;
    float phase = (1.0 - g * g) /
                  pow(max(1.0 + g * g - 2.0 * g * dot(direction, fx_fog_sun.xyz), 1.0e-4), 1.5);
    fog_light = (1.0 - fog_transmittance) * fx_sky.Load(int3(0, 0, 0)).rgb *
                (fx_fog_color.rgb + fx_fog_sun_color.rgb * phase);
  }

  float3 shafts = 0.0;
  if (fx_effect_flags & FX_EFFECT_VOLUMETRICS) {
    if (volumetric_full) {
      shafts = fx_volumetric.Load(int3(pixel, 0)).rgb;
    } else {
      [unroll] for (int i = 0; i < 4; ++i) {
        shafts += fx_volumetric.Load(int3(texels[i], 0)).rgb * weights[i];
      }
      shafts *= inv_weight_sum;
    }
  }
  // On the surface, so seen through the fog.
  float3 reflections = 0.0;
  if ((fx_effect_flags & FX_EFFECT_REFLECTIONS) && !is_sky) {
    float4 reflection;
    FX_UPSAMPLE(fx_reflections, reflection)
    reflections = reflection.rgb * (fx_effect_strengths.y * fog_transmittance);
  }
  if (fx_composite_mode == FX_COMPOSITE_ADD) {
    return float4(fog_light + shafts + reflections, 0.0);
  }

  float contact = 1.0;
  if ((fx_effect_flags & FX_EFFECT_CONTACT_SHADOWS) && !is_sky) {
    FX_UPSAMPLE(fx_contact_shadows, contact)
    contact = lerp(1.0, contact, fx_effect_strengths.x);
  }

  float ao = 1.0;
  if ((fx_effect_flags & FX_EFFECT_AO) && !is_sky) {
    float visibility;
    FX_UPSAMPLE(fx_ao, visibility)
    if (fx_ao_albedo > 0.0) {
      visibility = FxMultiBounce(visibility, fx_ao_albedo);
    }
    ao = lerp(1.0, visibility, fx_ao_strength);
  }

  // Indirect light reaching the surface, as a multiplier of its color: the
  // surface reflects albedo * light, approximated as its color * light /
  // (its brightness, but not below a fraction of the sky's).
  float3 indirect = 0.0;
  if ((fx_effect_flags & FX_EFFECT_GI) && !is_sky) {
    float3 light = 0.0;
    float3 surface = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
      if (!ao_full) {
        light += fx_gi.Load(int3(texels[i], 0)).rgb * weights[i];
      }
      // The captured scene is always at half resolution.
      surface += fx_scene_color.Load(int3(texels[i], 0)).rgb * weights[i];
    }
    light = ao_full ? fx_gi.Load(int3(pixel, 0)).rgb : light * inv_weight_sum;
    surface *= inv_weight_sum;
    float floor_brightness =
        max(0.1 * FxLuminance(fx_sky.Load(int3(0, 0, 0)).rgb), 1.0e-3);
    indirect = min(fx_gi_intensity * 0.5 * light / max(FxLuminance(surface), floor_brightness),
                   4.0);
  }

  if (fx_debug_mode == FX_DEBUG_AO) {
    return float4(ao, ao, ao, 1.0);
  }
  if (fx_debug_mode == FX_DEBUG_VOLUMETRICS) {
    return float4(shafts / (1.0 + shafts), 1.0);
  }
  if (fx_debug_mode == FX_DEBUG_GI) {
    return float4(indirect / (1.0 + indirect), 1.0);
  }
  if (fx_debug_mode == FX_DEBUG_FOG) {
    return float4(fog_light / (1.0 + fog_light), 1.0);
  }
  if (fx_debug_mode == FX_DEBUG_CONTACT_SHADOWS) {
    return float4(contact, contact, contact, 1.0);
  }
  if (fx_debug_mode == FX_DEBUG_REFLECTIONS) {
    return float4(reflections / (1.0 + reflections), 1.0);
  }
  return float4((ao * contact + indirect) * fog_transmittance, 1.0);
}
