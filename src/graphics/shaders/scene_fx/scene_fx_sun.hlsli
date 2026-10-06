// The sun and the guest's sun shadow cascades, for the volumetric lighting and
// the contact shadows (the extra constant buffer at binding `index`).

#ifndef SCENE_FX_SUN_HLSLI_
#define SCENE_FX_SUN_HLSLI_

#include "scene_fx_common.hlsli"

#define FX_SUN_CONSTANTS(index)                                                    \
  FX_EXTRA_CONSTANTS_BLOCK(index) {                                                \
    /* Per cascade, view space position to the shadow's clip space: x and y in  \
       -1...1, z = depth before the viewport transform. */                      \
    float4 fx_sun_cascade_x[2];                                                    \
    float4 fx_sun_cascade_y[2];                                                    \
    float4 fx_sun_cascade_z[2];                                                    \
    /* Per cascade: x = 1 / viewport depth scale, y = viewport depth offset,    \
       z = 1 if valid, w = depth bias. */                                       \
    float4 fx_sun_cascade_depth[2];                                                \
    /* Per cascade: xy = size in texels. */                                     \
    float4 fx_sun_cascade_size[2];                                                 \
    /* View space position to world height. */                                  \
    float4 fx_sun_height;                                                          \
    /* xyz = toward the sun in view space, w = Henyey-Greenstein anisotropy. */ \
    float4 fx_sun_direction;                                                       \
    /* Volumetric lighting: x = max distance, y = step count, z = intensity,    \
       w = haze density. */                                                     \
    float4 fx_sun_volumetric;                                                      \
    /* rgb = sun color relative to the sky; w = 1 to add the fog's density. */  \
    float4 fx_sun_tint;                                                            \
    /* The fog's density: x = at height y (world), z = falloff with height,     \
       w = everywhere. */                                                       \
    float4 fx_sun_fog;                                                             \
    /* Contact shadows: x = ray length, y = thickness, z = step count,          \
       w = strength. */                                                         \
    float4 fx_sun_contact;                                                         \
  };

// Fraction of the sun reaching the position through the cascade, filtered over
// the 2x2 nearest shadow texels (bilinear PCF); -1 outside of it.
float FxCascadeShadow(Texture2D<float> shadow_map, SamplerState point_clamp, float4 row_x,
                      float4 row_y, float4 row_z, float4 depth, float4 size, float3 position) {
  float4 p = float4(position, 1.0);
  float3 light = float3(dot(row_x, p), dot(row_y, p), dot(row_z, p));
  if (depth.z <= 0.0 || any(abs(light.xy) >= 0.98)) {
    return -1.0;
  }
  float2 texel = (light.xy * float2(0.5, -0.5) + 0.5) * size.xy - 0.5;
  float2 fraction = frac(texel);
  float2 gather_uv = (floor(texel) + 1.0) / size.xy;
  // (-,+), (+,+), (+,-), (-,-) of the 2x2 footprint.
  float4 occluders = (shadow_map.GatherRed(point_clamp, gather_uv) - depth.y) * depth.x;
  float4 lit = step(light.z, occluders + depth.w);
  return lerp(lerp(lit.w, lit.z, fraction.x), lerp(lit.x, lit.y, fraction.x), fraction.y);
}

// Through the finer cascade where it covers the position; 1 beyond both.
#define FX_SUN_SHADOW(shadow0, shadow1, point_clamp, position, result)                    \
  {                                                                                       \
    result = FxCascadeShadow(shadow0, point_clamp, fx_sun_cascade_x[0],                   \
                             fx_sun_cascade_y[0], fx_sun_cascade_z[0],                    \
                             fx_sun_cascade_depth[0], fx_sun_cascade_size[0], position);  \
    if (result < 0.0) {                                                                   \
      result = FxCascadeShadow(shadow1, point_clamp, fx_sun_cascade_x[1],                 \
                               fx_sun_cascade_y[1], fx_sun_cascade_z[1],                  \
                               fx_sun_cascade_depth[1], fx_sun_cascade_size[1], position);\
      result = result < 0.0 ? 1.0 : result;                                               \
    }                                                                                     \
  }

#endif  // SCENE_FX_SUN_HLSLI_
