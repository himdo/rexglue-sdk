// Ground-truth-based ambient occlusion with visibility bitmasks, at full or
// half resolution (fx_effect_scale): GTAO's cosine-weighted slice integral
// (Jimenez et al. 2016, XeGTAO) with occluders of finite thickness marking 32
// sectors per slice instead of a single horizon (Therrien et al. 2023, as in
// MXAO), so thin objects don't cast halos onto what's behind them.
// Output 1 = unoccluded.
//
// FX_GI also gathers one bounce of indirect light (screen-space global
// illumination, as in Therrien et al.): each sector a sample newly occludes
// receives that surface's light from the previous frame, reprojected.

#include "scene_fx_common.hlsli"

// Half resolution, 4 MIPs (texel i: scene pixel 2i at MIP 0).
FX_BINDING(0) Texture2D<float> fx_view_depth : register(t0);
// Full resolution.
FX_BINDING(1) Texture2D<float> fx_depth : register(t1);
#ifdef FX_GI
// Half resolution, the guest's scene before the effects, previous frame.
FX_BINDING(2) Texture2D<float4> fx_previous_color : register(t2);
FX_EXTRA_CONSTANTS_BLOCK(3) {
  // View space position this frame to the previous frame's clip space.
  float4 fx_gi_reproject[4];
};
FX_BINDING(4) FX_FORMAT_R16F RWTexture2D<float> fx_ao : register(u0);
FX_BINDING(5) FX_FORMAT_RGBA16F RWTexture2D<float4> fx_gi : register(u1);
#else
FX_BINDING(2) FX_FORMAT_R16F RWTexture2D<float> fx_ao : register(u0);
#endif

static const uint kFxSectorCount = 32;
static const int kFxHalfMipCount = 4;

// The view space position of the depth around a scene position at a level:
// -1 = full resolution, 0...3 = the half-resolution MIPs. Positioned where the
// depth was taken (MIP 0 texel i: scene pixel 2i; coarser texels: the mean of
// their MIP 0 sample positions).
float3 FxSampleViewPosition(float2 screen, int level) {
  if (level < 0) {
    int2 pixel = clamp(int2(screen), int2(0, 0), int2(fx_scene_size) - 1);
    return FxViewPosition(float2(pixel) + 0.5, fx_depth.Load(int3(pixel, 0)));
  }
  float texel_scale = float(2u << uint(level));
  int2 level_size = max(FxEffectSize(2u) >> level, int2(1, 1));
  int2 texel = clamp(int2(floor(screen / texel_scale)), int2(0, 0), level_size - 1);
  float2 sample_screen = float2(texel) * texel_scale + (0.5 * texel_scale - 0.5);
  return FxViewPosition(sample_screen, fx_view_depth.Load(int3(texel, level)));
}

// The center of an effect texel and its neighbors.
float3 FxCenterViewPosition(int2 texel) {
  texel = clamp(texel, int2(0, 0), FxEffectSize(fx_effect_scale) - 1);
  float2 screen = FxEffectScreen(texel, fx_effect_scale);
  float depth = fx_effect_scale == 1u ? fx_depth.Load(int3(texel, 0))
                                      : fx_view_depth.Load(int3(texel, 0));
  return FxViewPosition(screen, depth);
}

// Part of the cosine-weighted slice integral from 0 to h (signed angles from
// the view vector, n = projected normal angle): GTAO's IntegrateArc.
float FxIntegrateArc(float h, float n, float sin_n, float cos_n) {
  return (cos_n - cos(2.0 * h - n) + 2.0 * h * sin_n) * 0.25;
}

#ifdef FX_GI
// The light leaving a surface toward the camera last frame, 0 if it was off
// screen.
float3 FxPreviousRadiance(float3 position) {
  float4 p = float4(position, 1.0);
  float4 clip = float4(dot(fx_gi_reproject[0], p), dot(fx_gi_reproject[1], p),
                       dot(fx_gi_reproject[2], p), dot(fx_gi_reproject[3], p));
  if (clip.w <= 1.0e-4) {
    return 0.0;
  }
  float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
  if (any(uv < 0.0) || any(uv >= 1.0)) {
    return 0.0;
  }
  int2 texel = int2(uv * float2(FxEffectSize(2u)));
  return fx_previous_color.Load(int3(texel, 0)).rgb;
}
#endif

// Sectors are rounded to the nearest edge: rounding outward would make every
// sample on the surface's own plane (exactly on the hemisphere's border) mark
// a sector, darkening all flat surfaces.
uint FxSectorMask(float t_start, float t_end) {
  uint start = uint(saturate(t_start) * float(kFxSectorCount) + 0.5);
  uint end = uint(saturate(t_end) * float(kFxSectorCount) + 0.5);
  if (end <= start) {
    return 0u;
  }
  uint count = end - start;
  uint mask = count >= kFxSectorCount ? 0xFFFFFFFFu : ((1u << count) - 1u);
  return mask << start;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  int2 size = FxEffectSize(fx_effect_scale);
  if (any(int2(id.xy) >= size)) {
    return;
  }
  int2 texel = int2(id.xy);
  float3 position = FxCenterViewPosition(texel);
  if (position.z >= fx_sky_distance) {
    fx_ao[texel] = 1.0;
#ifdef FX_GI
    fx_gi[texel] = 0.0;
#endif
    return;
  }
  float2 screen = FxEffectScreen(texel, fx_effect_scale);

  // Normal from the depth, per axis from the side with the smaller depth step,
  // so silhouettes don't bend it.
  float3 right = FxCenterViewPosition(texel + int2(1, 0)) - position;
  float3 left = position - FxCenterViewPosition(texel - int2(1, 0));
  float3 down = FxCenterViewPosition(texel + int2(0, 1)) - position;
  float3 up = position - FxCenterViewPosition(texel - int2(0, 1));
  float3 dx = abs(right.z) < abs(left.z) ? right : left;
  float3 dy = abs(down.z) < abs(up.z) ? down : up;
  float3 normal = normalize(cross(dx, dy));
  float3 view = normalize(-position);
  if (dot(normal, view) < 0.0) {
    normal = -normal;
  }

  // Screen pixels per world unit at this distance (pixels needn't be square).
  float2 pixels_per_unit =
      0.5 * float2(fx_scene_size) / (position.z * float2(fx_inv_p00, fx_inv_p11));
  float noise_slice = FxNoise(float2(texel));
  float noise_step = FxNoise(float2(texel) + float2(41.0, 17.0));
  uint slice_count = max(fx_ao_slice_count, 1u);
  uint step_count = max(fx_ao_step_count, 1u);
  float radius_squared = fx_ao_radius * fx_ao_radius;
  // The finest depth level used, and the closest a sample can be (a texel).
  int finest_level = fx_effect_scale == 1u ? -1 : 0;
  float min_offset = float(fx_effect_scale);

  float visibility_sum = 0.0;
  float weight_sum = 0.0;
#ifdef FX_GI
  float3 light_sum = 0.0;
#endif
  for (uint slice = 0; slice < slice_count; ++slice) {
    float phi = (float(slice) + noise_slice) * (kFxPi / float(slice_count));
    float2 direction = float2(cos(phi), sin(phi));
    // The whole radius on screen along this view-space direction.
    float2 radius_screen = float2(direction.x, -direction.y) * pixels_per_unit * fx_ao_radius;
    float radius_pixels = length(radius_screen);
    if (radius_pixels > fx_ao_max_pixels) {
      radius_screen *= fx_ao_max_pixels / radius_pixels;
      radius_pixels = fx_ao_max_pixels;
    }

    // The slice plane: the view vector and the direction perpendicular to it.
    float3 direction_view = float3(direction, 0.0);
    float3 ortho = normalize(direction_view - dot(direction_view, view) * view);
    float3 axis = normalize(cross(direction_view, view));
    float3 projected_normal = normal - axis * dot(normal, axis);
    float projected_length = length(projected_normal);
    if (projected_length < 1.0e-4) {
      continue;
    }
    float cos_n = saturate(dot(projected_normal, view) / projected_length);
    float n = (dot(projected_normal, ortho) < 0.0 ? -1.0 : 1.0) * FxFastAcos(cos_n);
    float sin_n = sin(n);
    // The visible half circle is [n - pi/2, n + pi/2]; sectors split it
    // evenly by the cosine-weighted integral.
    float arc_low = FxIntegrateArc(n - kFxHalfPi, n, sin_n, cos_n);
    float arc_total = arc_low + FxIntegrateArc(n + kFxHalfPi, n, sin_n, cos_n);
    if (arc_total <= 1.0e-5) {
      continue;
    }
    float inv_arc_total = 1.0 / arc_total;

    uint occluded = 0u;
#ifdef FX_GI
    float3 slice_light = 0.0;
#endif
    if (radius_pixels >= min_offset) {
      for (uint step = 0; step < step_count; ++step) {
        // Denser near the center.
        float s = (float(step) + frac(noise_step + 0.618034 * float(slice))) / float(step_count);
        float offset_pixels = max(s * s * radius_pixels, min_offset + float(step));
        float2 offset_screen = radius_screen * (offset_pixels / radius_pixels);
        // Coarser depth further out, for the texture cache (XeGTAO).
        int level = max(int(clamp(log2(offset_pixels) - 3.3, 0.0, float(kFxHalfMipCount))) - 1,
                        finest_level);
        [unroll] for (uint side = 0; side < 2; ++side) {
          float2 sample_screen = screen + (side ? -offset_screen : offset_screen);
          // Nothing is known beyond the screen's edges.
          if (any(sample_screen < 0.0) || any(sample_screen >= float2(fx_scene_size))) {
            continue;
          }
          float3 sample_position = FxSampleViewPosition(sample_screen, level);
          float3 front = sample_position - position;
          float front_distance_squared = dot(front, front);
          if (front_distance_squared > radius_squared ||
              sample_position.z >= fx_sky_distance) {
            continue;
          }
          float3 back = front + normalize(sample_position) * fx_ao_thickness;
          // Signed angles from the view vector toward the slice direction.
          float front_cos = dot(front, view) * rsqrt(front_distance_squared + 1.0e-8);
          float back_cos = dot(back, view) * rsqrt(dot(back, back) + 1.0e-8);
          float front_angle = (dot(front, ortho) < 0.0 ? -1.0 : 1.0) * FxFastAcos(front_cos);
          float back_angle = (dot(back, ortho) < 0.0 ? -1.0 : 1.0) * FxFastAcos(back_cos);
          front_angle = clamp(front_angle, n - kFxHalfPi, n + kFxHalfPi);
          back_angle = clamp(back_angle, n - kFxHalfPi, n + kFxHalfPi);
          // To the cumulative weight across the half circle.
          float front_arc = FxIntegrateArc(front_angle, n, sin_n, cos_n);
          float back_arc = FxIntegrateArc(back_angle, n, sin_n, cos_n);
          float front_t = (front_angle <= 0.0 ? arc_low - front_arc : arc_low + front_arc) *
                          inv_arc_total;
          float back_t =
              (back_angle <= 0.0 ? arc_low - back_arc : arc_low + back_arc) * inv_arc_total;
          uint mask = FxSectorMask(min(front_t, back_t), max(front_t, back_t));
#ifdef FX_GI
          // The sectors this surface hides were lit by it.
          uint newly_occluded = mask & ~occluded;
          if (newly_occluded) {
            slice_light += FxPreviousRadiance(sample_position) *
                           (float(countbits(newly_occluded)) / float(kFxSectorCount));
          }
#endif
          occluded |= mask;
        }
      }
    }
    float slice_weight = projected_length * arc_total;
    visibility_sum +=
        slice_weight * (1.0 - float(countbits(occluded)) / float(kFxSectorCount));
#ifdef FX_GI
    light_sum += slice_weight * slice_light;
#endif
    weight_sum += slice_weight;
  }
  float visibility = weight_sum > 0.0 ? visibility_sum / weight_sum : 1.0;
  fx_ao[texel] = pow(saturate(visibility), fx_ao_power);
#ifdef FX_GI
  fx_gi[texel] = float4(weight_sum > 0.0 ? light_sum / weight_sum : 0.0, 1.0);
#endif
}
