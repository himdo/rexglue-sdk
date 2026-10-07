/**
 * @file        graphics/pipeline/scene_effects.cpp
 * @brief       Modern graphics effects layered onto the guest's own rendering,
 *              shared by the GPU backends
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/pipeline/scene_effects.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#define REX_SCENE_FX_LIVE .lifecycle(rex::cvar::Lifecycle::kHotReload)

// Ambient occlusion.
REXCVAR_DEFINE_BOOL(scene_fx_ao, true, "GPU/Effects",
                    "Ambient occlusion (GTAO with visibility bitmasks)") REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_INT32(scene_fx_ao_quality, 2, "GPU/Effects",
                     "Ambient occlusion quality: 0 = low, 1 = medium, 2 = high, 3 = ultra")
    .range(0, 3) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_ao_radius, 1.5, "GPU/Effects",
                      "Ambient occlusion radius in world units (meters)")
    .range(0.1, 10.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_ao_strength, 1.0, "GPU/Effects", "Ambient occlusion strength")
    .range(0.0, 1.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_ao_thickness, 0.5, "GPU/Effects",
                      "Assumed occluder thickness in world units - lower reduces darkening "
                      "behind thin objects")
    .range(0.01, 5.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_ao_power, 1.5, "GPU/Effects", "Ambient occlusion contrast")
    .range(0.5, 4.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_ao_albedo, 0.4, "GPU/Effects",
                      "Assumed surface albedo for multi-bounce compensation (0 = off)")
    .range(0.0, 0.9) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_BOOL(scene_fx_ao_full_resolution, false, "GPU/Effects",
                    "Compute the ambient occlusion, global illumination, contact shadows and "
                    "reflections at full resolution (sharper, about 4x the cost)")
    REX_SCENE_FX_LIVE;

// Global illumination.
REXCVAR_DEFINE_BOOL(scene_fx_gi, false, "GPU/Effects",
                    "Global illumination: one bounce of light between nearby surfaces")
    REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_gi_intensity, 1.0, "GPU/Effects", "Global illumination strength")
    .range(0.0, 4.0) REX_SCENE_FX_LIVE;

// Contact shadows.
REXCVAR_DEFINE_BOOL(scene_fx_contact_shadows, true, "GPU/Effects",
                    "Contact shadows: fine sun shadows the game's shadow maps miss")
    REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_contact_shadows_length, 0.5, "GPU/Effects",
                      "Contact shadow ray length in world units")
    .range(0.05, 3.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_contact_shadows_thickness, 0.3, "GPU/Effects",
                      "Assumed thickness of what casts contact shadows, in world units")
    .range(0.02, 2.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_contact_shadows_strength, 0.6, "GPU/Effects",
                      "Contact shadow darkness")
    .range(0.0, 1.0) REX_SCENE_FX_LIVE;

// Screen-space reflections.
REXCVAR_DEFINE_BOOL(scene_fx_reflections, false, "GPU/Effects",
                    "Screen-space reflections (the game doesn't say which surfaces are "
                    "glossy, so all reflect a little, most at grazing angles)")
    REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_INT32(scene_fx_reflections_quality, 2, "GPU/Effects",
                     "Reflection quality: 0 = low, 1 = medium, 2 = high, 3 = ultra")
    .range(0, 3) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_reflections_intensity, 1.0, "GPU/Effects", "Reflection strength")
    .range(0.0, 4.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_reflections_reflectance, 0.04, "GPU/Effects",
                      "Reflectance seen head-on (Fresnel F0) - more makes everything shinier")
    .range(0.0, 1.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_reflections_distance, 30.0, "GPU/Effects",
                      "Longest reflected distance, in world units")
    .range(1.0, 200.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_reflections_thickness, 0.5, "GPU/Effects",
                      "Assumed thickness of reflected objects, in world units")
    .range(0.05, 5.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_BOOL(scene_fx_reflections_floors_only, false, "GPU/Effects",
                    "Reflect only on upward-facing surfaces (floors, wet streets)")
    REX_SCENE_FX_LIVE;

// Volumetric lighting.
REXCVAR_DEFINE_BOOL(scene_fx_volumetrics, true, "GPU/Effects",
                    "Volumetric lighting: light shafts through the game's sun shadows")
    REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_INT32(scene_fx_volumetrics_quality, 2, "GPU/Effects",
                     "Volumetric lighting quality: 0 = low, 1 = medium, 2 = high, 3 = ultra")
    .range(0, 3) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_volumetrics_intensity, 1.0, "GPU/Effects",
                      "Light shaft brightness, relative to the sky's")
    .range(0.0, 8.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_volumetrics_density, 0.004, "GPU/Effects",
                      "Haze the light shafts are seen in, per world unit (the fog adds to it)")
    .range(0.0, 0.1) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_volumetrics_anisotropy, 0.6, "GPU/Effects",
                      "How much the air scatters light forward (glow toward the sun)")
    .range(0.0, 0.95) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_volumetrics_distance, 120.0, "GPU/Effects",
                      "Distance the light shafts extend to, in world units")
    .range(10.0, 1000.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_BOOL(scene_fx_volumetrics_temporal, true, "GPU/Effects",
                    "Accumulate the light shafts over frames (smoother, less noise)")
    REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_BOOL(scene_fx_volumetrics_full_resolution, false, "GPU/Effects",
                    "Compute the light shafts at full resolution (about 4x the cost)")
    REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_STRING(scene_fx_sun_tint, "1.0,0.9,0.75", "GPU/Effects",
                      "Color of scattered sunlight relative to the sky's (r,g,b)")
    REX_SCENE_FX_LIVE;

// Fog.
REXCVAR_DEFINE_BOOL(scene_fx_fog, true, "GPU/Effects",
                    "Height fog lit by the sky, with a glow toward the sun") REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_fog_density, 0.0005, "GPU/Effects",
                      "Fog density everywhere, per world unit")
    .range(0.0, 0.1) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_fog_ground_density, 0.003, "GPU/Effects",
                      "Ground fog density at its height, per world unit")
    .range(0.0, 0.5) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_fog_ground_height, -2.0, "GPU/Effects",
                      "Height of the ground fog relative to the camera, in world units")
    .range(-50.0, 50.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_fog_ground_falloff, 0.15, "GPU/Effects",
                      "How quickly the ground fog thins out with height, per world unit")
    .range(0.01, 5.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_fog_brightness, 1.0, "GPU/Effects",
                      "Fog brightness relative to the sky")
    .range(0.0, 4.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_fog_sun_glow, 0.5, "GPU/Effects",
                      "Sunlight the fog scatters (glow toward the sun)")
    .range(0.0, 4.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_fog_distance, 1000.0, "GPU/Effects",
                      "Distance of the sky through the fog, in world units")
    .range(50.0, 20000.0) REX_SCENE_FX_LIVE;

// Image.
REXCVAR_DEFINE_BOOL(scene_fx_sharpen, false, "GPU/Effects",
                    "Contrast-adaptive sharpening of the final image") REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_sharpen_amount, 0.5, "GPU/Effects", "Sharpening strength")
    .range(0.0, 1.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_BOOL(scene_fx_grading, false, "GPU/Effects", "Color grading of the final image")
    REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_grading_exposure, 1.0, "GPU/Effects", "Brightness multiplier")
    .range(0.25, 4.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_grading_contrast, 1.0, "GPU/Effects", "Contrast")
    .range(0.5, 2.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_grading_saturation, 1.0, "GPU/Effects", "Saturation")
    .range(0.0, 2.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_grading_vibrance, 0.0, "GPU/Effects",
                      "Vibrance - saturates muted colors more than vivid ones")
    .range(-1.0, 1.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_grading_temperature, 0.0, "GPU/Effects",
                      "White balance: negative cooler, positive warmer")
    .range(-1.0, 1.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_grading_tint, 0.0, "GPU/Effects",
                      "White balance: negative greener, positive more magenta")
    .range(-1.0, 1.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_grading_gamma, 1.0, "GPU/Effects",
                      "Gamma: above 1 brightens the midtones")
    .range(0.5, 2.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_grading_shadow_lift, 0.0, "GPU/Effects",
                      "Raises the darkest parts of the image, leaving the brights")
    .range(0.0, 0.5) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_vignette, 0.0, "GPU/Effects",
                      "Darkening toward the screen's corners")
    .range(0.0, 1.0) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_DOUBLE(scene_fx_film_grain, 0.0, "GPU/Effects", "Film grain")
    .range(0.0, 1.0) REX_SCENE_FX_LIVE;

// Common.
REXCVAR_DEFINE_INT32(scene_fx_camera_constant, 0, "GPU/Effects",
                     "First of the four vertex shader float constants holding the (world-) "
                     "view-projection rows in the depth passes (-1: default projection)")
    .range(-1, 252) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_INT32(scene_fx_world_constant, 4, "GPU/Effects",
                     "First of the three vertex shader float constants holding the object's "
                     "world transform rows, which the camera constants include and are freed "
                     "of (-1: the camera constants are the view-projection alone)")
    .range(-1, 253) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_INT32(scene_fx_debug, 0, "GPU/Effects",
                     "Effects debug view: 0 = off, 1 = effects on the left half of the screen "
                     "only, 2 = ambient occlusion, 3 = volumetric lighting, 4 = global "
                     "illumination, 5 = fog, 6 = contact shadows, 7 = reflections")
    .range(0, 7) REX_SCENE_FX_LIVE;
REXCVAR_DEFINE_BOOL(scene_fx_trace, false, "GPU/Effects",
                    "Log the effects' decisions for one frame, then reset") REX_SCENE_FX_LIVE;

namespace rex::graphics {

namespace {

// Matches FxConstants in scene_fx_common.hlsli.
struct FxConstants {
  float proj_a;
  float proj_b;
  float inv_p00;
  float inv_p11;
  uint32_t scene_size[2];
  int32_t screen_offset[2];
  float ao_radius;
  float ao_strength;
  float ao_thickness;
  float sky_distance;
  uint32_t blur_direction[2];
  uint32_t frame;
  uint32_t debug_mode;
  float output_scale[2];
  uint32_t ao_slice_count;
  uint32_t ao_step_count;
  float ao_power;
  float ao_albedo;
  float ao_max_pixels;
  uint32_t effect_flags;
  uint32_t pass_rect[4];
  float gi_intensity;
  uint32_t composite_mode;
  uint32_t effect_scale;
  uint32_t volumetric_scale;
};
static_assert(sizeof(FxConstants) == 128);

// Matches FX_SUN_CONSTANTS in scene_fx_sun.hlsli.
struct SunConstants {
  float cascade_x[2][4];
  float cascade_y[2][4];
  float cascade_z[2][4];
  float cascade_depth[2][4];
  float cascade_size[2][4];
  float height[4];
  float direction[4];
  float volumetric[4];
  float tint[4];
  float fog[4];
  float contact[4];
};

struct TemporalConstants {
  float reproject[4][4];
  float params[4];
};

struct ReflectionConstants {
  float reproject[4][4];
  float up[4];
  float params[4];
};

struct ImageConstants {
  float sharpen[4];
  float grade[4];
  float white[4];
  float finish[4];
};

// fx_effect_flags, scene_fx_debug and fx_composite_mode, as in
// scene_fx_common.hlsli.
constexpr uint32_t kEffectAo = 1;
constexpr uint32_t kEffectVolumetrics = 2;
constexpr uint32_t kEffectGi = 4;
constexpr uint32_t kEffectFog = 8;
constexpr uint32_t kEffectContactShadows = 16;
constexpr uint32_t kEffectReflections = 32;
constexpr int32_t kDebugSplit = 1;
constexpr uint32_t kCompositeMultiply = 0;
constexpr uint32_t kCompositeAdd = 1;
constexpr uint32_t kCompositeDebug = 2;

// Slices x steps per side - without temporal accumulation.
struct AoQuality {
  uint32_t slice_count;
  uint32_t step_count;
};
constexpr AoQuality kAoQualities[] = {{2, 3}, {3, 4}, {4, 6}, {6, 8}};
constexpr uint32_t kVolumetricStepCounts[] = {16, 24, 40, 64};
constexpr uint32_t kReflectionStepCounts[] = {16, 24, 32, 48};
constexpr uint32_t kContactShadowStepCount = 16;
// Weight of the newest frame in the volumetric lighting's history.
constexpr float kTemporalBlend = 0.1f;

template <typename T>
T QualityEntry(const T (&table)[4], int32_t quality) {
  return table[std::clamp(quality, 0, 3)];
}

bool IsHdrColorFormat(xenos::ColorRenderTargetFormat format) {
  return format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
         format == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16 ||
         format == xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT;
}

float Length3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

uint32_t EffectSize(uint32_t size, uint32_t scale) { return (size + scale - 1) / scale; }

}  // namespace

const SceneEffects::TextureInfo
    SceneEffects::kTextureInfos[size_t(SceneEffects::Texture::kCount)] = {
        {TextureFormat::kR32Float, 1, false},                   // kDepth
        {TextureFormat::kR32Float, kViewDepthMipCount, false},  // kViewDepth
        {TextureFormat::kR16Float, 1, false},                   // kAo0
        {TextureFormat::kR16Float, 1, false},                   // kAo1
        {TextureFormat::kRGBA16Float, 1, false},                // kGi0
        {TextureFormat::kRGBA16Float, 1, false},                // kGi1
        {TextureFormat::kR16Float, 1, false},                   // kContactShadows0
        {TextureFormat::kR16Float, 1, false},                   // kContactShadows1
        {TextureFormat::kRGBA16Float, 1, false},                // kReflections0
        {TextureFormat::kRGBA16Float, 1, false},                // kReflections1
        {TextureFormat::kRGBA16Float, 1, false},                // kVolumetric
        {TextureFormat::kRGBA16Float, 1, false},                // kVolumetricBlur
        {TextureFormat::kRGBA16Float, 1, false},                // kVolumetricHistory0
        {TextureFormat::kRGBA16Float, 1, false},                // kVolumetricHistory1
        {TextureFormat::kRGBA16Float, 1, false},                // kSceneColor0
        {TextureFormat::kRGBA16Float, 1, false},                // kSceneColor1
        {TextureFormat::kRGBA16Float, 1, true},                 // kSky0
        {TextureFormat::kRGBA16Float, 1, true},                 // kSky1
        {TextureFormat::kR32Float, 1, false},                   // kShadow0
        {TextureFormat::kR32Float, 1, false},                   // kShadow1
        {TextureFormat::kRGBA16Float, 1, false},                // kFinalImage
};

const SceneEffects::PipelineInfo
    SceneEffects::kPipelineInfos[size_t(SceneEffects::Pipeline::kCount)] = {
        {1, false, false, 1},  // kDepthCopy
        {1, false, false, 1},  // kDepthCopyMsaa
        {1, false, false, 1},  // kShadowCopy
        {1, false, false, 4},  // kDepthPrefilter
        {2, false, false, 1},  // kAo
        {3, false, true, 2},   // kAoGi
        {2, false, false, 1},  // kBlur
        {2, false, false, 1},  // kBlurRgba
        {3, false, false, 1},  // kSkyColor
        {3, false, false, 1},  // kSkyColorMsaa
        {1, false, false, 1},  // kColorCapture
        {1, false, false, 1},  // kColorCaptureMsaa
        {1, false, false, 1},  // kColorCopy
        {1, false, false, 1},  // kColorCopyMsaa
        {4, true, true, 1},    // kContactShadows
        {3, true, true, 1},    // kReflections
        {4, true, true, 1},    // kVolumetric
        {3, true, true, 1},    // kTemporal
};

SceneEffects::SceneEffects(RenderTargetCache& render_target_cache,
                           const RegisterFile& register_file)
    : render_target_cache_(render_target_cache), register_file_(register_file) {
  // Until the guest's camera is seen: Fable II's usual gameplay projection.
  const float near_z = 0.1f, far_z = 5000.0f;
  camera_.proj_a = far_z / (far_z - near_z);
  camera_.proj_b = -near_z * camera_.proj_a;
  camera_.inv_p00 = 1.0f / 0.714f;
  camera_.inv_p11 = 1.0f / 1.269f;
  camera_.has_matrix = false;
  shadow_cascades_[0].texture = Texture::kShadow0;
  shadow_cascades_[1].texture = Texture::kShadow1;
}

SceneEffects::~SceneEffects() = default;

bool SceneEffects::AnySceneEffectEnabled() {
  return REXCVAR_GET(scene_fx_ao) || REXCVAR_GET(scene_fx_gi) ||
         REXCVAR_GET(scene_fx_contact_shadows) || REXCVAR_GET(scene_fx_reflections) ||
         REXCVAR_GET(scene_fx_volumetrics) || REXCVAR_GET(scene_fx_fog);
}

bool SceneEffects::AnyImageEffectEnabled() {
  return REXCVAR_GET(scene_fx_sharpen) || REXCVAR_GET(scene_fx_grading);
}

bool SceneEffects::SunShadowsNeeded() {
  return REXCVAR_GET(scene_fx_volumetrics) || REXCVAR_GET(scene_fx_contact_shadows) ||
         REXCVAR_GET(scene_fx_fog);
}

void SceneEffects::OnDraw() {
  if (!AnySceneEffectEnabled()) {
    return;
  }
  // Only depth-writing draws define the depth being resolved.
  auto depth_control = register_file_.Get<reg::RB_DEPTHCONTROL>();
  if (!depth_control.z_enable || !depth_control.z_write_enable) {
    return;
  }
  DrawState state;
  ReadCameraConstants(state.rows);
  auto vte_control = register_file_.Get<reg::PA_CL_VTE_CNTL>();
  state.depth_range.scale = vte_control.vport_z_scale_ena
                                ? register_file_.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE)
                                : 1.0f;
  state.depth_range.offset = vte_control.vport_z_offset_ena
                                 ? register_file_.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET)
                                 : 0.0f;
  // Freed of different world transforms, the same camera differs in rounding.
  auto nearly_equal = [](const float(&a)[4][4], const float(&b)[4][4]) {
    for (uint32_t i = 0; i < 4; ++i) {
      for (uint32_t j = 0; j < 4; ++j) {
        if (std::abs(a[i][j] - b[i][j]) > 1.0e-4f * std::max(1.0f, std::abs(a[i][j]))) {
          return false;
        }
      }
    }
    return true;
  };
  for (uint32_t i = 0; i < draw_state_count_; ++i) {
    DrawState& existing = draw_states_[i];
    if (existing.depth_range.scale == state.depth_range.scale &&
        existing.depth_range.offset == state.depth_range.offset &&
        nearly_equal(existing.rows, state.rows)) {
      ++existing.count;
      return;
    }
  }
  state.count = 1;
  if (draw_state_count_ < kMaxDrawStates) {
    draw_states_[draw_state_count_++] = state;
    return;
  }
  // Full: replace a one-off.
  for (DrawState& existing : draw_states_) {
    if (existing.count == 1) {
      existing = state;
      return;
    }
  }
}

const SceneEffects::DrawState* SceneEffects::GetDominantDrawState(bool orthographic) const {
  const DrawState* dominant = nullptr;
  for (uint32_t i = 0; i < draw_state_count_; ++i) {
    const DrawState& state = draw_states_[i];
    if ((orthographic ? IsOrthographic(state.rows) : IsPerspective(state.rows)) &&
        (!dominant || state.count > dominant->count)) {
      dominant = &state;
    }
  }
  return dominant;
}

void SceneEffects::OnResolve() {
  // The draw states counted are of the pass being resolved - start over for
  // the next one.
  struct DrawStatesReset {
    uint32_t& count;
    ~DrawStatesReset() { count = 0; }
  } draw_states_reset{draw_state_count_};

  uint64_t frame = GetCurrentFrame();
  // scene_fx_trace logs the decisions over the next 3 whole frames.
  if (REXCVAR_GET(scene_fx_trace) && trace_frame_ == UINT64_MAX) {
    trace_frame_ = frame + 1;
    trace_frame_end_ = frame + 4;
  } else if (trace_frame_ != UINT64_MAX && frame >= trace_frame_end_) {
    trace_frame_ = UINT64_MAX;
    rex::cvar::SetFlagByName("scene_fx_trace", "false");
  }
  bool trace = frame >= trace_frame_ && frame < trace_frame_end_;

  bool scene_effects = AnySceneEffectEnabled();
  int32_t debug_mode = REXCVAR_GET(scene_fx_debug);
  if ((!scene_effects && !AnyImageEffectEnabled() && debug_mode <= kDebugSplit) ||
      render_target_cache_.GetPath() != RenderTargetCache::Path::kHostRenderTargets) {
    if (trace) {
      REXGPU_INFO("Scene effects trace: disabled or not using host render targets");
    }
    return;
  }

  auto copy_control = register_file_.Get<reg::RB_COPY_CONTROL>();
  auto dest_pitch = register_file_.Get<reg::RB_COPY_DEST_PITCH>();
  uint32_t scale_x = render_target_cache_.draw_resolution_scale_x();
  uint32_t scale_y = render_target_cache_.draw_resolution_scale_y();
  uint32_t width = dest_pitch.copy_dest_pitch * scale_x;
  uint32_t height = dest_pitch.copy_dest_height * scale_y;
  // Screen pixel = render target pixel + this. Predicated tiling renders each
  // tile at the top of eDRAM via a negative window offset.
  auto window_offset = register_file_.Get<reg::PA_SC_WINDOW_OFFSET>();
  int32_t screen_offset_x = -int32_t(window_offset.window_x_offset) * int32_t(scale_x);
  int32_t screen_offset_y = -int32_t(window_offset.window_y_offset) * int32_t(scale_y);
  RenderTarget* const* render_targets =
      render_target_cache_.last_update_accumulated_render_targets();
  bool is_depth = copy_control.copy_src_select >= xenos::kMaxColorRenderTargets;
  RenderTarget* source_rt = render_targets[is_depth ? 0 : 1 + copy_control.copy_src_select];
  if (trace) {
    REXGPU_INFO("Scene effects trace: frame {} resolve of {} {}x{}, screen offset {},{}, source {}",
                frame, is_depth ? "depth" : "color", width, height, screen_offset_x,
                screen_offset_y, source_rt ? source_rt->key().GetDebugName() : "none");
  }
  if (!source_rt) {
    return;
  }

  if (is_depth) {
    if (!scene_effects) {
      return;
    }
    const DrawState* shadow_state = GetDominantDrawState(true);
    const DrawState* scene_state = GetDominantDrawState(false);
    if (trace) {
      REXGPU_INFO("Scene effects trace: {} draw states, dominant orthographic {}, perspective {}",
                  draw_state_count_, shadow_state ? shadow_state->count : 0,
                  scene_state ? scene_state->count : 0);
    }
    if (shadow_state && (!scene_state || shadow_state->count >= scene_state->count)) {
      // A sun shadow map - drawn with the same shaders as the scene's depth.
      if (SunShadowsNeeded() && source_rt->key().msaa_samples == xenos::MsaaSamples::k1X) {
        CaptureShadowCascade(*source_rt, *shadow_state, width, height, trace);
      }
      return;
    }
    // Full scenes only - not reflections or other smaller views.
    if (dest_pitch.copy_dest_pitch < 640 || dest_pitch.copy_dest_height < 360 ||
        !EnsureSceneTextures(width, height)) {
      return;
    }
    if (scene_state) {
      scene_depth_range_ = scene_state->depth_range;
      CaptureCamera(scene_state->rows);
    }
    if (trace) {
      CameraBasis basis;
      bool has_basis = camera_.has_matrix && GetCameraBasis(camera_.rows, basis);
      REXGPU_INFO(
          "Scene effects trace: scene depth, camera A {} B {} inverse P00 {} P11 {}, depth scale "
          "{} offset {}, at {:.2f} {:.2f} {:.2f}",
          camera_.proj_a, camera_.proj_b, camera_.inv_p00, camera_.inv_p11,
          scene_depth_range_.scale, scene_depth_range_.offset,
          has_basis ? basis.position[0] : 0.0f, has_basis ? basis.position[1] : 0.0f,
          has_basis ? basis.position[2] : 0.0f);
    }
    scene_width_ = width;
    scene_height_ = height;
    FxConstants constants;
    FillConstants(&constants, screen_offset_x, screen_offset_y);
    bool msaa = source_rt->key().msaa_samples != xenos::MsaaSamples::k1X;
    if (Dispatch(msaa ? Pipeline::kDepthCopyMsaa : Pipeline::kDepthCopy, {Source(source_rt)},
                 {Target(Texture::kDepth)}, &constants, nullptr, 0, width, height)) {
      depth_frame_ = frame;
    }
    return;
  }

  // Color resolves after the scene depth in the same frame.
  if (depth_frame_ != frame) {
    if (trace) {
      REXGPU_INFO("Scene effects trace: no scene depth this frame yet");
    }
    return;
  }
  // The resolved region in render target pixels: the scissor, moved by the
  // window offset.
  auto scissor_tl = register_file_.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  auto scissor_br = register_file_.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  Rect rect;
  rect.left = std::max(0, (int32_t(scissor_tl.tl_x) + int32_t(window_offset.window_x_offset)) *
                              int32_t(scale_x));
  rect.top = std::max(0, (int32_t(scissor_tl.tl_y) + int32_t(window_offset.window_y_offset)) *
                             int32_t(scale_y));
  rect.right = (int32_t(scissor_br.br_x) + int32_t(window_offset.window_x_offset)) * int32_t(scale_x);
  rect.bottom =
      (int32_t(scissor_br.br_y) + int32_t(window_offset.window_y_offset)) * int32_t(scale_y);
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return;
  }

  if (IsHdrColorFormat(source_rt->key().GetColorFormat())) {
    // HDR color of the scene: composite the effects before it's resolved, so
    // the guest's own post-processing (bloom, exposure, tonemapping) and UI
    // come on top.
    if (width != scene_width_ || height != scene_height_) {
      if (trace) {
        REXGPU_INFO("Scene effects trace: HDR color not of the scene's size ({}x{})", scene_width_,
                    scene_height_);
      }
      return;
    }
    hdr_frame_ = frame;
    if (!scene_effects) {
      return;
    }
    if (effects_frame_ != frame) {
      ComputeEffects(*source_rt, rect, screen_offset_x, screen_offset_y, trace);
    }
    if (REXCVAR_GET(scene_fx_gi) || REXCVAR_GET(scene_fx_reflections)) {
      CaptureSceneColor(*source_rt, rect, screen_offset_x, screen_offset_y);
    }
    if (debug_mode > kDebugSplit) {
      // Shown over the final image instead.
      return;
    }
    if (ao_computed_ || gi_computed_ || contact_shadows_computed_ || fog_enabled_) {
      Composite(*source_rt, rect, screen_offset_x, screen_offset_y, 1.0f, 1.0f,
                Draw::kCompositeMultiply, trace);
    }
    if (fog_enabled_ || volumetrics_computed_ || reflections_computed_) {
      Composite(*source_rt, rect, screen_offset_x, screen_offset_y, 1.0f, 1.0f,
                Draw::kCompositeAdd, trace);
    }
    return;
  }

  // The first scene-sized resolve after the HDR scene's: the final image.
  if (hdr_frame_ != frame || image_frame_ == frame || width < scene_width_ ||
      height < scene_height_) {
    return;
  }
  image_frame_ = frame;
  ApplyImageEffects(*source_rt, rect, width, height, trace);
  if (debug_mode > kDebugSplit && effects_frame_ == frame) {
    // An effect alone, without the guest's tonemapping.
    Composite(*source_rt, rect, 0, 0, float(scene_width_) / float(width),
              float(scene_height_) / float(height), Draw::kCompositeDebug, trace);
  }
}

void SceneEffects::ReadCameraConstants(float rows[4][4]) const {
  int32_t base = REXCVAR_GET(scene_fx_camera_constant);
  if (base < 0) {
    std::memset(rows, 0, sizeof(float) * 16);
    return;
  }
  auto constant = [this](int32_t index) {
    return reinterpret_cast<const float*>(
        &register_file_.values[XE_GPU_REG_SHADER_CONSTANT_000_X + index * 4]);
  };
  for (uint32_t row = 0; row < 4; ++row) {
    std::memcpy(rows[row], constant(base + row), sizeof(float) * 4);
  }

  // Games often pass world-view-projection (WVP = VP * W) per object, with
  // the world transform W separately: VP = WVP * W^-1, the same for every
  // object, so the whole pass agrees on its camera.
  int32_t world_base = REXCVAR_GET(scene_fx_world_constant);
  if (world_base < 0) {
    return;
  }
  const float* w[3] = {constant(world_base), constant(world_base + 1), constant(world_base + 2)};
  // Rotation with uniform scale (rows orthogonal and of equal length), and a
  // translation in w.
  float scale_squared = Dot3(w[0], w[0]);
  if (scale_squared < 1.0e-8f) {
    return;
  }
  float tolerance = 1.0e-3f * scale_squared;
  if (std::abs(Dot3(w[1], w[1]) - scale_squared) > tolerance ||
      std::abs(Dot3(w[2], w[2]) - scale_squared) > tolerance ||
      std::abs(Dot3(w[0], w[1])) > tolerance || std::abs(Dot3(w[0], w[2])) > tolerance ||
      std::abs(Dot3(w[1], w[2])) > tolerance) {
    return;
  }
  // W^-1 = [M^T / s^2 | -M^T t / s^2] for W = [M | t] with M = s R.
  float inverse[3][4];
  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t j = 0; j < 3; ++j) {
      inverse[i][j] = w[j][i] / scale_squared;
    }
    inverse[i][3] =
        -(inverse[i][0] * w[0][3] + inverse[i][1] * w[1][3] + inverse[i][2] * w[2][3]);
  }
  for (uint32_t row = 0; row < 4; ++row) {
    float wvp[4];
    std::memcpy(wvp, rows[row], sizeof(wvp));
    for (uint32_t column = 0; column < 4; ++column) {
      rows[row][column] = wvp[0] * inverse[0][column] + wvp[1] * inverse[1][column] +
                          wvp[2] * inverse[2][column] + (column == 3 ? wvp[3] : 0.0f);
    }
  }
}

bool SceneEffects::IsOrthographic(const float rows[4][4]) {
  return std::abs(rows[3][0]) < 1.0e-6f && std::abs(rows[3][1]) < 1.0e-6f &&
         std::abs(rows[3][2]) < 1.0e-6f && std::abs(rows[3][3] - 1.0f) < 1.0e-4f &&
         Length3(rows[0]) > 1.0e-6f && Length3(rows[1]) > 1.0e-6f && Length3(rows[2]) > 1.0e-7f;
}

bool SceneEffects::GetCameraBasis(const float rows[4][4], CameraBasis& basis_out) {
  float p00 = Length3(rows[0]), p11 = Length3(rows[1]), w_length = Length3(rows[3]);
  if (p00 <= 0.0f || p11 <= 0.0f || w_length <= 0.0f) {
    return false;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    basis_out.right[i] = rows[0][i] / p00;
    basis_out.up[i] = rows[1][i] / p11;
    basis_out.forward[i] = rows[3][i] / w_length;
  }
  float translation[3] = {rows[0][3] / p00, rows[1][3] / p11, rows[3][3] / w_length};
  for (uint32_t i = 0; i < 3; ++i) {
    basis_out.position[i] = -(translation[0] * basis_out.right[i] +
                              translation[1] * basis_out.up[i] +
                              translation[2] * basis_out.forward[i]);
  }
  basis_out.up_axis = 0;
  for (uint32_t i = 1; i < 3; ++i) {
    if (std::abs(basis_out.up[i]) > std::abs(basis_out.up[basis_out.up_axis])) {
      basis_out.up_axis = i;
    }
  }
  basis_out.up_sign = basis_out.up[basis_out.up_axis] >= 0.0f ? 1.0f : -1.0f;
  return true;
}

bool SceneEffects::IsPerspective(const float rows[4][4]) {
  float length_x = Length3(rows[0]), length_y = Length3(rows[1]);
  float length_z = Length3(rows[2]), length_w = Length3(rows[3]);
  // A perspective view-projection (with at most a rigid world transform): the
  // w row is the camera's unit forward axis and the z row is parallel to it
  // (z = A * w + B).
  return std::abs(length_w - 1.0f) < 0.01f && length_x > 0.05f && length_y > 0.05f &&
         length_z > 0.5f && Dot3(rows[2], rows[3]) > 0.999f * length_z * length_w;
}

void SceneEffects::CaptureCamera(const float rows[4][4]) {
  if (!IsPerspective(rows)) {
    return;
  }
  float length_x = Length3(rows[0]), length_y = Length3(rows[1]);
  float length_z = Length3(rows[2]), length_w = Length3(rows[3]);
  camera_.proj_a = length_z / length_w;
  camera_.proj_b = rows[2][3] - camera_.proj_a * rows[3][3];
  camera_.inv_p00 = 1.0f / length_x;
  camera_.inv_p11 = 1.0f / length_y;
  camera_.has_matrix = true;
  std::memcpy(camera_.rows, rows, sizeof(camera_.rows));
  if (!camera_logged_) {
    camera_logged_ = true;
    float a, b;
    GetDepthToDistance(a, b);
    REXGPU_INFO(
        "Scene effects: camera at stored depth 0 {:.3f}, at 1 {:.3f}, vertical field of view "
        "{:.1f} degrees (viewport depth scale {}, offset {})",
        b / -a, b / (1.0f - a), 2.0f * std::atan(camera_.inv_p11) * 180.0f / 3.14159265f,
        scene_depth_range_.scale, scene_depth_range_.offset);
  }
}

void SceneEffects::CaptureShadowCascade(RenderTarget& depth_rt, const DrawState& draw_state,
                                        uint32_t width, uint32_t height, bool trace) {
  if (width < 128 || height < 128) {
    return;
  }
  uint64_t frame = GetCurrentFrame();
  float coverage = 2.0f / Length3(draw_state.rows[0]);
  // The slot of the cascade of about this size, otherwise an unused one - the
  // cascades are told apart by size, as games may update them in turns.
  int32_t slot = -1;
  for (uint32_t i = 0; i < kMaxShadowCascades && slot < 0; ++i) {
    const ShadowCascade& cascade = shadow_cascades_[i];
    if (cascade.valid && std::abs(cascade.coverage - coverage) < 0.25f * coverage) {
      slot = int32_t(i);
    }
  }
  for (uint32_t i = 0; i < kMaxShadowCascades && slot < 0; ++i) {
    if (!IsShadowCascadeUsable(shadow_cascades_[i])) {
      slot = int32_t(i);
    }
  }
  if (slot < 0) {
    return;
  }
  ShadowCascade& cascade = shadow_cascades_[slot];
  if (!EnsureTexture(cascade.texture, width, height)) {
    return;
  }
  FxConstants constants;
  FillConstants(&constants, 0, 0);
  constants.scene_size[0] = width;
  constants.scene_size[1] = height;
  if (!Dispatch(Pipeline::kShadowCopy, {Source(&depth_rt)}, {Target(cascade.texture)},
                &constants, nullptr, 0, width, height)) {
    return;
  }
  std::memcpy(cascade.rows, draw_state.rows, sizeof(cascade.rows));
  cascade.depth_range = draw_state.depth_range;
  cascade.width = width;
  cascade.height = height;
  cascade.coverage = coverage;
  cascade.frame = frame;
  cascade.valid = true;
  // The finer cascade first (the shaders use it where it covers).
  static_assert(kMaxShadowCascades == 2);
  if (shadow_cascades_[0].valid && shadow_cascades_[1].valid &&
      shadow_cascades_[0].coverage > shadow_cascades_[1].coverage) {
    std::swap(shadow_cascades_[0], shadow_cascades_[1]);
  }
  if (trace) {
    REXGPU_INFO("Scene effects trace: shadow cascade ({}x{}, {:.1f} units wide, {} draws)",
                width, height, coverage, draw_state.count);
  }
}

bool SceneEffects::IsShadowCascadeUsable(const ShadowCascade& cascade) const {
  return cascade.valid && GetCurrentFrame() - cascade.frame <= kShadowCascadeMaxAge;
}

uint32_t SceneEffects::GetShadowCascadeCount() const {
  uint32_t count = 0;
  for (const ShadowCascade& cascade : shadow_cascades_) {
    count += IsShadowCascadeUsable(cascade) ? 1 : 0;
  }
  return count;
}

void SceneEffects::GetDepthToDistance(float& a_out, float& b_out) const {
  // stored = offset + scale * (A + B / z), so z = scale * B / (stored - (offset + scale * A)).
  a_out = scene_depth_range_.offset + scene_depth_range_.scale * camera_.proj_a;
  b_out = scene_depth_range_.scale * camera_.proj_b;
}

bool SceneEffects::EnsureSceneTextures(uint32_t width, uint32_t height) {
  if (texture_width_ < width || texture_height_ < height) {
    texture_width_ = std::max(texture_width_, width);
    texture_height_ = std::max(texture_height_, height);
    // The previous frame's results are lost with their textures.
    scene_color_valid_[0] = scene_color_valid_[1] = false;
    volumetric_history_valid_ = false;
  }
  uint32_t half_width = EffectSize(texture_width_, 2);
  uint32_t half_height = EffectSize(texture_height_, 2);
  for (uint32_t i = 0; i < uint32_t(Texture::kCount); ++i) {
    Texture texture = Texture(i);
    uint32_t texture_width = texture_width_, texture_height = texture_height_;
    switch (texture) {
      case Texture::kViewDepth:
      case Texture::kSceneColor0:
      case Texture::kSceneColor1:
        texture_width = half_width;
        texture_height = half_height;
        break;
      case Texture::kSky0:
      case Texture::kSky1:
        texture_width = texture_height = 1;
        break;
      case Texture::kShadow0:
      case Texture::kShadow1:
      case Texture::kFinalImage:
        // Their own sizes.
        continue;
      default:
        break;
    }
    if (!EnsureTexture(texture, texture_width, texture_height)) {
      REXGPU_ERROR("Scene effects: failed to create {}x{} textures", texture_width_,
                   texture_height_);
      return false;
    }
  }
  return true;
}

void SceneEffects::FillConstants(void* constants_out, int32_t screen_offset_x,
                                 int32_t screen_offset_y) const {
  FxConstants constants = {};
  GetDepthToDistance(constants.proj_a, constants.proj_b);
  constants.inv_p00 = camera_.inv_p00;
  constants.inv_p11 = camera_.inv_p11;
  constants.scene_size[0] = scene_width_;
  constants.scene_size[1] = scene_height_;
  constants.screen_offset[0] = screen_offset_x;
  constants.screen_offset[1] = screen_offset_y;
  constants.ao_radius = float(REXCVAR_GET(scene_fx_ao_radius));
  constants.ao_strength = float(REXCVAR_GET(scene_fx_ao_strength));
  constants.ao_thickness = float(REXCVAR_GET(scene_fx_ao_thickness));
  // Cleared depth (the sky) is at the far plane - stored 1, or 0 if reversed.
  float distance_at_0 = constants.proj_b / -constants.proj_a;
  float distance_at_1 = constants.proj_b / (1.0f - constants.proj_a);
  constants.sky_distance = 0.95f * std::max(distance_at_0, distance_at_1);
  constants.frame = uint32_t(GetCurrentFrame());
  constants.debug_mode = uint32_t(REXCVAR_GET(scene_fx_debug));
  constants.output_scale[0] = 1.0f;
  constants.output_scale[1] = 1.0f;
  AoQuality quality = QualityEntry(kAoQualities, REXCVAR_GET(scene_fx_ao_quality));
  constants.ao_slice_count = quality.slice_count;
  constants.ao_step_count = quality.step_count;
  constants.ao_power = float(REXCVAR_GET(scene_fx_ao_power));
  constants.ao_albedo = float(REXCVAR_GET(scene_fx_ao_albedo));
  // Bounds the cost and the texture cache footprint of close-up occlusion.
  constants.ao_max_pixels = 0.25f * float(scene_height_);
  constants.gi_intensity = float(REXCVAR_GET(scene_fx_gi_intensity));
  constants.effect_scale = ao_scale_;
  constants.volumetric_scale = volumetric_scale_;
  std::memcpy(constants_out, &constants, sizeof(constants));
}

bool SceneEffects::GetReprojection(float reproject_out[4][4]) const {
  CameraBasis basis;
  if (!previous_camera_valid_ || !camera_.has_matrix || !GetCameraBasis(camera_.rows, basis)) {
    return false;
  }
  for (uint32_t row = 0; row < 4; ++row) {
    const float* p = previous_camera_[row];
    reproject_out[row][0] = Dot3(p, basis.right);
    reproject_out[row][1] = Dot3(p, basis.up);
    reproject_out[row][2] = Dot3(p, basis.forward);
    reproject_out[row][3] = Dot3(p, basis.position) + p[3];
  }
  return true;
}

bool SceneEffects::GetHeightRow(const CameraBasis& basis, float height_out[4]) const {
  uint32_t axis = basis.up_axis;
  height_out[0] = basis.right[axis] * basis.up_sign;
  height_out[1] = basis.up[axis] * basis.up_sign;
  height_out[2] = basis.forward[axis] * basis.up_sign;
  height_out[3] = basis.position[axis] * basis.up_sign;
  return true;
}

bool SceneEffects::GetSunDirection(const CameraBasis& basis, float toward_sun_out[3]) const {
  // Against a shadow cascade's depth axis (depth grows away from the light);
  // straight up without shadows.
  float sun_world[3] = {0.0f, 0.0f, 0.0f};
  const ShadowCascade* cascade = nullptr;
  for (const ShadowCascade& candidate : shadow_cascades_) {
    if (!cascade && IsShadowCascadeUsable(candidate)) {
      cascade = &candidate;
    }
  }
  if (cascade) {
    const float* depth_axis = cascade->rows[2];
    float length = Length3(depth_axis);
    for (uint32_t i = 0; i < 3; ++i) {
      sun_world[i] = -depth_axis[i] / length;
    }
  } else {
    sun_world[basis.up_axis] = basis.up_sign;
  }
  bool from_shadows = cascade != nullptr;
  toward_sun_out[0] = Dot3(sun_world, basis.right);
  toward_sun_out[1] = Dot3(sun_world, basis.up);
  toward_sun_out[2] = Dot3(sun_world, basis.forward);
  return from_shadows;
}

void SceneEffects::GetSunTint(float tint_out[3]) {
  // "r,g,b"; missing components stay 1.
  tint_out[0] = tint_out[1] = tint_out[2] = 1.0f;
  const char* tint = REXCVAR_GET(scene_fx_sun_tint).c_str();
  for (uint32_t i = 0; i < 3 && *tint; ++i) {
    char* end;
    float component = std::strtof(tint, &end);
    if (end == tint) {
      break;
    }
    tint_out[i] = std::max(component, 0.0f);
    tint = end;
    while (*tint == ',' || *tint == ' ') {
      ++tint;
    }
  }
}

void SceneEffects::FillSunConstants(const CameraBasis& basis, void* constants_out,
                                    Source shadow_sources_out[kMaxShadowCascades]) const {
  SunConstants sc = {};
  for (uint32_t i = 0; i < kMaxShadowCascades; ++i) {
    const ShadowCascade& cascade = shadow_cascades_[i];
    // Unusable cascades aren't sampled, but need a valid view.
    bool usable = IsShadowCascadeUsable(cascade);
    shadow_sources_out[i] = usable ? Source(cascade.texture) : Source(Texture::kDepth);
    if (!usable) {
      continue;
    }
    float(*out[3])[4] = {sc.cascade_x, sc.cascade_y, sc.cascade_z};
    for (uint32_t row = 0; row < 3; ++row) {
      const float* l = cascade.rows[row];
      out[row][i][0] = Dot3(l, basis.right);
      out[row][i][1] = Dot3(l, basis.up);
      out[row][i][2] = Dot3(l, basis.forward);
      out[row][i][3] = Dot3(l, basis.position) + l[3];
    }
    float scale = cascade.depth_range.scale;
    sc.cascade_depth[i][0] = std::abs(scale) > 1.0e-6f ? 1.0f / scale : 1.0f;
    sc.cascade_depth[i][1] = cascade.depth_range.offset;
    sc.cascade_depth[i][2] = 1.0f;
    sc.cascade_depth[i][3] = 0.002f;
    sc.cascade_size[i][0] = float(cascade.width);
    sc.cascade_size[i][1] = float(cascade.height);
  }
  GetHeightRow(basis, sc.height);
  GetSunDirection(basis, sc.direction);
  sc.direction[3] = float(REXCVAR_GET(scene_fx_volumetrics_anisotropy));
  sc.volumetric[0] = float(REXCVAR_GET(scene_fx_volumetrics_distance));
  sc.volumetric[1] =
      float(QualityEntry(kVolumetricStepCounts, REXCVAR_GET(scene_fx_volumetrics_quality)));
  sc.volumetric[2] = float(REXCVAR_GET(scene_fx_volumetrics_intensity));
  sc.volumetric[3] = float(REXCVAR_GET(scene_fx_volumetrics_density));
  GetSunTint(sc.tint);
  // The shafts are seen in the fog too.
  bool fog = REXCVAR_GET(scene_fx_fog);
  sc.tint[3] = fog ? 1.0f : 0.0f;
  sc.fog[0] = float(REXCVAR_GET(scene_fx_fog_ground_density));
  sc.fog[1] = sc.height[3] + float(REXCVAR_GET(scene_fx_fog_ground_height));
  sc.fog[2] = float(REXCVAR_GET(scene_fx_fog_ground_falloff));
  sc.fog[3] = float(REXCVAR_GET(scene_fx_fog_density));
  sc.contact[0] = float(REXCVAR_GET(scene_fx_contact_shadows_length));
  sc.contact[1] = float(REXCVAR_GET(scene_fx_contact_shadows_thickness));
  sc.contact[2] = float(kContactShadowStepCount);
  sc.contact[3] = float(REXCVAR_GET(scene_fx_contact_shadows_strength));
  std::memcpy(constants_out, &sc, sizeof(sc));
}

void SceneEffects::ComputeEffects(RenderTarget& color_rt, const Rect& rect,
                                  int32_t screen_offset_x, int32_t screen_offset_y, bool trace) {
  uint64_t frame = GetCurrentFrame();
  effects_frame_ = frame;
  // This frame's scene goes to the other color buffer - the light source of
  // the next frame's global illumination and reflections.
  scene_color_index_ ^= 1;
  scene_color_valid_[scene_color_index_] = false;
  ao_scale_ = REXCVAR_GET(scene_fx_ao_full_resolution) ? 1 : 2;
  volumetric_scale_ = REXCVAR_GET(scene_fx_volumetrics_full_resolution) ? 1 : 2;
  bool ao = REXCVAR_GET(scene_fx_ao);
  bool gi = REXCVAR_GET(scene_fx_gi);
  bool fog = REXCVAR_GET(scene_fx_fog);
  bool volumetrics = REXCVAR_GET(scene_fx_volumetrics);

  PrefilterDepth();
  if (volumetrics || fog || gi) {
    EstimateSkyColor(color_rt, rect, screen_offset_x, screen_offset_y);
  }
  ao_computed_ = false;
  gi_computed_ = false;
  if (ao || gi) {
    gi_computed_ = ComputeAmbientOcclusion(gi);
    ao_computed_ = ao;
  }
  contact_shadows_computed_ = REXCVAR_GET(scene_fx_contact_shadows) && ComputeContactShadows();
  reflections_computed_ = REXCVAR_GET(scene_fx_reflections) && ComputeReflections();
  volumetrics_computed_ = volumetrics && ComputeVolumetrics();
  fog_enabled_ = FillFogConstants() && fog;

  // For the next frame's reprojection.
  if (camera_.has_matrix) {
    std::memcpy(previous_camera_, camera_.rows, sizeof(previous_camera_));
    previous_camera_valid_ = true;
  }
  if (trace) {
    REXGPU_INFO(
        "Scene effects trace: computed AO {}, GI {}, contact shadows {}, reflections {}, "
        "volumetrics {}, fog {} ({} shadow cascades)",
        ao_computed_, gi_computed_, contact_shadows_computed_, reflections_computed_,
        volumetrics_computed_, fog_enabled_, GetShadowCascadeCount());
  }
}

void SceneEffects::PrefilterDepth() {
  FxConstants constants;
  FillConstants(&constants, 0, 0);
  static_assert(kViewDepthMipCount == 4);
  Dispatch(Pipeline::kDepthPrefilter, {Source(Texture::kDepth)},
           {Target(Texture::kViewDepth, 0), Target(Texture::kViewDepth, 1),
            Target(Texture::kViewDepth, 2), Target(Texture::kViewDepth, 3)},
           &constants, nullptr, 0, EffectSize(scene_width_, 2), EffectSize(scene_height_, 2));
}

bool SceneEffects::ComputeAmbientOcclusion(bool global_illumination) {
  FxConstants constants;
  FillConstants(&constants, 0, 0);
  uint32_t width = EffectSize(scene_width_, ao_scale_);
  uint32_t height = EffectSize(scene_height_, ao_scale_);
  // The global illumination's light source is the previous frame's scene,
  // reprojected.
  Texture previous_color = scene_color_index_ ? Texture::kSceneColor0 : Texture::kSceneColor1;
  float reproject[4][4];
  bool gi = global_illumination && scene_color_valid_[scene_color_index_ ^ 1] &&
            GetReprojection(reproject) &&
            Dispatch(Pipeline::kAoGi,
                     {Source(Texture::kViewDepth), Source(Texture::kDepth), Source(previous_color)},
                     {Target(Texture::kAo0), Target(Texture::kGi0)}, &constants, reproject,
                     sizeof(reproject), width, height);
  if (!gi) {
    Dispatch(Pipeline::kAo, {Source(Texture::kViewDepth), Source(Texture::kDepth)},
             {Target(Texture::kAo0)}, &constants, nullptr, 0, width, height);
  }
  if (REXCVAR_GET(scene_fx_ao)) {
    Blur(Texture::kAo0, Texture::kAo1, false, ao_scale_);
  }
  if (gi) {
    Blur(Texture::kGi0, Texture::kGi1, true, ao_scale_);
  }
  return gi;
}

bool SceneEffects::ComputeContactShadows() {
  CameraBasis basis;
  // Toward the sun, which only the shadow maps tell.
  if (!camera_.has_matrix || !GetShadowCascadeCount() || !GetCameraBasis(camera_.rows, basis)) {
    return false;
  }
  SunConstants sun;
  Source shadows[kMaxShadowCascades];
  FillSunConstants(basis, &sun, shadows);
  FxConstants constants;
  FillConstants(&constants, 0, 0);
  constants.effect_scale = ao_scale_;
  Texture effect_depth = ao_scale_ == 1 ? Texture::kDepth : Texture::kViewDepth;
  static_assert(kMaxShadowCascades == 2);
  if (!Dispatch(Pipeline::kContactShadows,
                {Source(effect_depth), Source(Texture::kDepth), shadows[0], shadows[1]},
                {Target(Texture::kContactShadows0)}, &constants, &sun, sizeof(sun),
                EffectSize(scene_width_, ao_scale_), EffectSize(scene_height_, ao_scale_))) {
    return false;
  }
  Blur(Texture::kContactShadows0, Texture::kContactShadows1, false, ao_scale_);
  return true;
}

bool SceneEffects::ComputeReflections() {
  CameraBasis basis;
  ReflectionConstants rc = {};
  if (!scene_color_valid_[scene_color_index_ ^ 1] || !GetReprojection(rc.reproject) ||
      !GetCameraBasis(camera_.rows, basis)) {
    return false;
  }
  float height[4];
  GetHeightRow(basis, height);
  rc.up[0] = height[0];
  rc.up[1] = height[1];
  rc.up[2] = height[2];
  rc.up[3] = REXCVAR_GET(scene_fx_reflections_floors_only) ? 1.0f : 0.0f;
  rc.params[0] = float(REXCVAR_GET(scene_fx_reflections_distance));
  rc.params[1] =
      float(QualityEntry(kReflectionStepCounts, REXCVAR_GET(scene_fx_reflections_quality)));
  rc.params[2] = float(REXCVAR_GET(scene_fx_reflections_thickness));
  rc.params[3] = float(REXCVAR_GET(scene_fx_reflections_reflectance));
  FxConstants constants;
  FillConstants(&constants, 0, 0);
  constants.effect_scale = ao_scale_;
  Texture previous_color = scene_color_index_ ? Texture::kSceneColor0 : Texture::kSceneColor1;
  if (!Dispatch(Pipeline::kReflections,
                {Source(Texture::kViewDepth), Source(Texture::kDepth), Source(previous_color)},
                {Target(Texture::kReflections0)}, &constants, &rc, sizeof(rc),
                EffectSize(scene_width_, ao_scale_), EffectSize(scene_height_, ao_scale_))) {
    return false;
  }
  Blur(Texture::kReflections0, Texture::kReflections1, true, ao_scale_);
  return true;
}

bool SceneEffects::ComputeVolumetrics() {
  CameraBasis basis;
  if (!camera_.has_matrix || !GetCameraBasis(camera_.rows, basis)) {
    return false;
  }
  SunConstants sun;
  Source shadows[kMaxShadowCascades];
  FillSunConstants(basis, &sun, shadows);
  FxConstants constants;
  FillConstants(&constants, 0, 0);
  constants.effect_scale = volumetric_scale_;
  uint32_t width = EffectSize(scene_width_, volumetric_scale_);
  uint32_t height = EffectSize(scene_height_, volumetric_scale_);
  Texture effect_depth = volumetric_scale_ == 1 ? Texture::kDepth : Texture::kViewDepth;
  Texture sky = sky_index_ ? Texture::kSky1 : Texture::kSky0;
  if (!Dispatch(Pipeline::kVolumetric, {Source(effect_depth), shadows[0], shadows[1], Source(sky)},
                {Target(Texture::kVolumetric)}, &constants, &sun, sizeof(sun), width, height)) {
    return false;
  }
  if (!REXCVAR_GET(scene_fx_volumetrics_temporal)) {
    Blur(Texture::kVolumetric, Texture::kVolumetricBlur, true, volumetric_scale_);
    volumetric_result_ = Texture::kVolumetric;
    volumetric_history_valid_ = false;
    return true;
  }
  // Accumulated into the other history texture.
  uint32_t current = volumetric_history_index_ ^ 1;
  Texture history_previous =
      volumetric_history_index_ ? Texture::kVolumetricHistory1 : Texture::kVolumetricHistory0;
  Texture history_current = current ? Texture::kVolumetricHistory1 : Texture::kVolumetricHistory0;
  TemporalConstants tc = {};
  bool history = volumetric_history_valid_ && volumetric_history_scale_ == volumetric_scale_ &&
                 GetReprojection(tc.reproject);
  tc.params[0] = history ? kTemporalBlend : 1.0f;
  tc.params[1] = float(REXCVAR_GET(scene_fx_volumetrics_distance));
  if (!Dispatch(Pipeline::kTemporal,
                {Source(Texture::kVolumetric), Source(history_previous), Source(effect_depth)},
                {Target(history_current)}, &constants, &tc, sizeof(tc), width, height)) {
    volumetric_result_ = Texture::kVolumetric;
    volumetric_history_valid_ = false;
    return true;
  }
  volumetric_history_index_ = current;
  volumetric_history_valid_ = true;
  volumetric_history_scale_ = volumetric_scale_;
  volumetric_result_ = history_current;
  return true;
}

bool SceneEffects::FillFogConstants() {
  // The composite's extra constants, with the fog's when it's on.
  std::memset(composite_constants_, 0, sizeof(composite_constants_));
  composite_constants_[5][0] = float(REXCVAR_GET(scene_fx_contact_shadows_strength));
  composite_constants_[5][1] = float(REXCVAR_GET(scene_fx_reflections_intensity));
  CameraBasis basis;
  if (!camera_.has_matrix || !GetCameraBasis(camera_.rows, basis)) {
    return false;
  }
  GetHeightRow(basis, composite_constants_[0]);
  composite_constants_[1][0] = float(REXCVAR_GET(scene_fx_fog_ground_density));
  composite_constants_[1][1] =
      composite_constants_[0][3] + float(REXCVAR_GET(scene_fx_fog_ground_height));
  composite_constants_[1][2] = float(REXCVAR_GET(scene_fx_fog_ground_falloff));
  composite_constants_[1][3] = float(REXCVAR_GET(scene_fx_fog_density));
  float brightness = float(REXCVAR_GET(scene_fx_fog_brightness));
  composite_constants_[2][0] = composite_constants_[2][1] = composite_constants_[2][2] =
      brightness;
  composite_constants_[2][3] = float(REXCVAR_GET(scene_fx_fog_distance));
  bool sun = GetSunDirection(basis, composite_constants_[3]);
  composite_constants_[3][3] = 0.75f;
  float tint[3];
  GetSunTint(tint);
  float glow = sun ? float(REXCVAR_GET(scene_fx_fog_sun_glow)) : 0.0f;
  for (uint32_t i = 0; i < 3; ++i) {
    composite_constants_[4][i] = tint[i] * glow;
  }
  return true;
}

void SceneEffects::EstimateSkyColor(RenderTarget& color_rt, const Rect& rect,
                                    int32_t screen_offset_x, int32_t screen_offset_y) {
  Texture previous = sky_index_ ? Texture::kSky1 : Texture::kSky0;
  Texture next = sky_index_ ? Texture::kSky0 : Texture::kSky1;
  FxConstants constants;
  FillConstants(&constants, screen_offset_x, screen_offset_y);
  constants.pass_rect[0] = uint32_t(rect.left);
  constants.pass_rect[1] = uint32_t(rect.top);
  constants.pass_rect[2] = uint32_t(rect.right);
  constants.pass_rect[3] = uint32_t(rect.bottom);
  bool msaa = color_rt.key().msaa_samples != xenos::MsaaSamples::k1X;
  // One 16x16 group.
  if (Dispatch(msaa ? Pipeline::kSkyColorMsaa : Pipeline::kSkyColor,
               {Source(&color_rt), Source(Texture::kDepth), Source(previous)}, {Target(next)},
               &constants, nullptr, 0, 1, 1)) {
    sky_index_ ^= 1;
  }
}

void SceneEffects::CaptureSceneColor(RenderTarget& color_rt, const Rect& rect,
                                     int32_t screen_offset_x, int32_t screen_offset_y) {
  FxConstants constants;
  FillConstants(&constants, screen_offset_x, screen_offset_y);
  constants.pass_rect[0] = uint32_t(rect.left);
  constants.pass_rect[1] = uint32_t(rect.top);
  constants.pass_rect[2] = uint32_t(rect.right);
  constants.pass_rect[3] = uint32_t(rect.bottom);
  bool msaa = color_rt.key().msaa_samples != xenos::MsaaSamples::k1X;
  Texture target = scene_color_index_ ? Texture::kSceneColor1 : Texture::kSceneColor0;
  // Half-resolution texels covering the tile (one more for odd edges).
  if (Dispatch(msaa ? Pipeline::kColorCaptureMsaa : Pipeline::kColorCapture,
               {Source(&color_rt)}, {Target(target)}, &constants, nullptr, 0,
               uint32_t(rect.right - rect.left) / 2 + 1,
               uint32_t(rect.bottom - rect.top) / 2 + 1)) {
    scene_color_valid_[scene_color_index_] = true;
  }
}

void SceneEffects::Blur(Texture texture_0, Texture texture_1, bool rgba, uint32_t scale) {
  FxConstants constants;
  FillConstants(&constants, 0, 0);
  constants.effect_scale = scale;
  Texture effect_depth = scale == 1 ? Texture::kDepth : Texture::kViewDepth;
  uint32_t width = EffectSize(scene_width_, scale), height = EffectSize(scene_height_, scale);
  Pipeline pipeline = rgba ? Pipeline::kBlurRgba : Pipeline::kBlur;
  // Horizontal, then vertical, ending in texture_0.
  constants.blur_direction[0] = 1;
  constants.blur_direction[1] = 0;
  Dispatch(pipeline, {Source(texture_0), Source(effect_depth)}, {Target(texture_1)}, &constants,
           nullptr, 0, width, height);
  constants.blur_direction[0] = 0;
  constants.blur_direction[1] = 1;
  Dispatch(pipeline, {Source(texture_1), Source(effect_depth)}, {Target(texture_0)}, &constants,
           nullptr, 0, width, height);
}

void SceneEffects::Composite(RenderTarget& color_rt, const Rect& rect, int32_t screen_offset_x,
                             int32_t screen_offset_y, float output_scale_x, float output_scale_y,
                             Draw draw, bool trace) {
  if (trace) {
    static const char* const kDrawNames[] = {"multiply", "add", "debug view", "image"};
    REXGPU_INFO("Scene effects trace: composite ({}) into {}-{},{}-{}",
                kDrawNames[uint32_t(draw)], rect.left, rect.right, rect.top, rect.bottom);
  }
  FxConstants constants;
  FillConstants(&constants, screen_offset_x, screen_offset_y);
  constants.output_scale[0] = output_scale_x;
  constants.output_scale[1] = output_scale_y;
  constants.effect_flags = (ao_computed_ ? kEffectAo : 0) | (gi_computed_ ? kEffectGi : 0) |
                           (volumetrics_computed_ ? kEffectVolumetrics : 0) |
                           (fog_enabled_ ? kEffectFog : 0) |
                           (contact_shadows_computed_ ? kEffectContactShadows : 0) |
                           (reflections_computed_ ? kEffectReflections : 0);
  constants.composite_mode = draw == Draw::kCompositeAdd   ? kCompositeAdd
                             : draw == Draw::kCompositeDebug ? kCompositeDebug
                                                             : kCompositeMultiply;
  Texture scene_color = scene_color_index_ ? Texture::kSceneColor1 : Texture::kSceneColor0;
  Texture sky = sky_index_ ? Texture::kSky1 : Texture::kSky0;
  // In the order of the composite shader's t0...
  DrawFullscreen(draw, color_rt, rect,
                 {Source(Texture::kAo0), Source(Texture::kViewDepth), Source(Texture::kDepth),
                  Source(volumetric_result_), Source(Texture::kGi0), Source(scene_color),
                  Source(sky), Source(Texture::kContactShadows0), Source(Texture::kReflections0)},
                 &constants, composite_constants_, sizeof(composite_constants_));
}

void SceneEffects::ApplyImageEffects(RenderTarget& color_rt, const Rect& rect, uint32_t width,
                                     uint32_t height, bool trace) {
  bool sharpen = REXCVAR_GET(scene_fx_sharpen);
  bool grading = REXCVAR_GET(scene_fx_grading);
  if (!sharpen && !grading) {
    return;
  }
  // The image, copied to read its neighborhoods.
  if (!EnsureTexture(Texture::kFinalImage, width, height)) {
    return;
  }
  FxConstants constants;
  FillConstants(&constants, 0, 0);
  constants.pass_rect[0] = uint32_t(rect.left);
  constants.pass_rect[1] = uint32_t(rect.top);
  constants.pass_rect[2] = uint32_t(rect.right);
  constants.pass_rect[3] = uint32_t(rect.bottom);
  bool msaa = color_rt.key().msaa_samples != xenos::MsaaSamples::k1X;
  if (!Dispatch(msaa ? Pipeline::kColorCopyMsaa : Pipeline::kColorCopy, {Source(&color_rt)},
                {Target(Texture::kFinalImage)}, &constants, nullptr, 0,
                uint32_t(rect.right - rect.left), uint32_t(rect.bottom - rect.top))) {
    return;
  }
  ImageConstants ic = {};
  ic.sharpen[0] = float(REXCVAR_GET(scene_fx_sharpen_amount));
  ic.sharpen[1] = sharpen ? 1.0f : 0.0f;
  ic.grade[0] = ic.grade[1] = ic.grade[2] = 1.0f;
  ic.white[0] = ic.white[1] = ic.white[2] = ic.white[3] = 1.0f;
  if (grading) {
    ic.grade[0] = float(REXCVAR_GET(scene_fx_grading_exposure));
    ic.grade[1] = float(REXCVAR_GET(scene_fx_grading_contrast));
    ic.grade[2] = float(REXCVAR_GET(scene_fx_grading_saturation));
    ic.grade[3] = float(REXCVAR_GET(scene_fx_grading_vibrance));
    // White balance: temperature along blue - amber, tint along green -
    // magenta.
    float temperature = float(REXCVAR_GET(scene_fx_grading_temperature));
    float tint = float(REXCVAR_GET(scene_fx_grading_tint));
    ic.white[0] = 1.0f + 0.1f * temperature + 0.05f * tint;
    ic.white[1] = 1.0f - 0.1f * tint;
    ic.white[2] = 1.0f - 0.1f * temperature + 0.05f * tint;
    ic.white[3] = 1.0f / float(REXCVAR_GET(scene_fx_grading_gamma));
    ic.finish[0] = float(REXCVAR_GET(scene_fx_grading_shadow_lift));
    ic.finish[1] = float(REXCVAR_GET(scene_fx_vignette));
    ic.finish[2] = float(REXCVAR_GET(scene_fx_film_grain));
  }
  if (trace) {
    REXGPU_INFO("Scene effects trace: image effects (sharpen {}, grading {}) on {}x{}", sharpen,
                grading, width, height);
  }
  DrawFullscreen(Draw::kImage, color_rt, rect, {Source(Texture::kFinalImage)}, &constants, &ic,
                 sizeof(ic));
}

}  // namespace rex::graphics
