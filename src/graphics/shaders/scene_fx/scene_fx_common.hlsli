// Scene effects shared declarations. Compiled with FXC (D3D12, shader model
// 5.1) and with glslang (Vulkan SPIR-V, SCENE_FX_VULKAN defined) - see
// tools/build_scene_fx_shaders.py in the game repository.

#ifndef SCENE_FX_COMMON_HLSLI_
#define SCENE_FX_COMMON_HLSLI_

#ifdef SCENE_FX_VULKAN
#define FX_BINDING(index) [[vk::binding(index, 0)]]
// A register would also decorate the push constant block with a binding.
#define FX_CONSTANTS_BLOCK [[vk::push_constant]] cbuffer FxConstants
#define FX_EXTRA_CONSTANTS_BLOCK(index) [[vk::binding(index, 0)]] cbuffer FxExtraConstants
// Storage image formats must match their views'.
#define FX_FORMAT_R32F [[spv::format_r32f]]
#define FX_FORMAT_R16F [[spv::format_r16f]]
#define FX_FORMAT_RGBA16F [[spv::format_rgba16f]]
#else
#define FX_BINDING(index)
#define FX_CONSTANTS_BLOCK cbuffer FxConstants : register(b0)
#define FX_EXTRA_CONSTANTS_BLOCK(index) cbuffer FxExtraConstants : register(b1)
#define FX_FORMAT_R32F
#define FX_FORMAT_R16F
#define FX_FORMAT_RGBA16F
#endif

// fx_effect_flags.
#define FX_EFFECT_AO 1u
#define FX_EFFECT_VOLUMETRICS 2u
#define FX_EFFECT_GI 4u
#define FX_EFFECT_FOG 8u
#define FX_EFFECT_CONTACT_SHADOWS 16u
#define FX_EFFECT_REFLECTIONS 32u

// scene_fx_debug.
#define FX_DEBUG_SPLIT 1u
#define FX_DEBUG_AO 2u
#define FX_DEBUG_VOLUMETRICS 3u
#define FX_DEBUG_GI 4u
#define FX_DEBUG_FOG 5u
#define FX_DEBUG_CONTACT_SHADOWS 6u
#define FX_DEBUG_REFLECTIONS 7u

// fx_composite_mode.
#define FX_COMPOSITE_MULTIPLY 0u  // dest * (occlusion + indirect) * fog transmittance
#define FX_COMPOSITE_ADD 1u       // dest + scattered light (fog, light shafts)
#define FX_COMPOSITE_DEBUG 2u     // replaces dest

// 32 dwords (128 bytes): D3D12 root constants / Vulkan push constants.
FX_CONSTANTS_BLOCK {
  // Stored device depth d to view distance z = fx_proj_b / (d - fx_proj_a).
  float fx_proj_a;
  float fx_proj_b;
  // View-space x = NDC x * z * fx_inv_p00 (and y with fx_inv_p11).
  float fx_inv_p00;
  float fx_inv_p11;
  // Full-resolution scene size in host pixels.
  uint2 fx_scene_size;
  // Screen pixel = render target pixel + fx_screen_offset (predicated tiling).
  int2 fx_screen_offset;
  float fx_ao_radius;     // world units
  float fx_ao_strength;   // 0 = off, 1 = full
  float fx_ao_thickness;  // assumed occluder thickness, world units
  float fx_sky_distance;  // view distance at and beyond which nothing occludes
  uint2 fx_blur_direction;
  uint fx_frame;
  uint fx_debug_mode;
  // Composite: scene pixel = SV_Position * fx_output_scale + fx_screen_offset.
  float2 fx_output_scale;
  uint fx_ao_slice_count;
  uint fx_ao_step_count;
  float fx_ao_power;       // contrast of the visibility
  float fx_ao_albedo;      // multi-bounce compensation, 0 = off
  float fx_ao_max_pixels;  // screen-space radius limit, full-resolution pixels
  uint fx_effect_flags;
  // Per pass: a render target rectangle (sky color, color capture).
  uint4 fx_pass_rect;
  float fx_gi_intensity;
  uint fx_composite_mode;
  // Scene pixels per effect texel (1 = full resolution, 2 = half): of this
  // pass, or in the composite, of the ambient occlusion and the global
  // illumination.
  uint fx_effect_scale;
  // In the composite, of the volumetric lighting.
  uint fx_volumetric_scale;
};

float FxLuminance(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }

int2 FxEffectSize(uint scale) { return int2((fx_scene_size + scale - 1) / scale); }

// Effect texel i was computed at scene pixel i * scale (the pixel's center).
float2 FxEffectScreen(int2 texel, uint scale) { return float2(texel * int(scale)) + 0.5; }

static const float kFxPi = 3.14159265;
static const float kFxHalfPi = 1.57079633;

float FxViewDistance(float device_depth) {
  return fx_proj_b / (device_depth - fx_proj_a);
}

// screen in full-resolution scene pixels (pixel centers at .5), y down.
float3 FxViewPosition(float2 screen, float distance) {
  float2 ndc = screen / float2(fx_scene_size) * float2(2.0, -2.0) + float2(-1.0, 1.0);
  return float3(ndc.x * distance * fx_inv_p00, ndc.y * distance * fx_inv_p11, distance);
}

// Jorge Jimenez's interleaved gradient noise.
float FxNoise(float2 pixel) {
  return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Lagarde's acos approximation, as used by XeGTAO.
float FxFastAcos(float x) {
  float result = -0.156583 * abs(x) + kFxHalfPi;
  result *= sqrt(1.0 - abs(x));
  return x >= 0.0 ? result : kFxPi - result;
}

#endif  // SCENE_FX_COMMON_HLSLI_
