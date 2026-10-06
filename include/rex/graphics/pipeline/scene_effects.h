/**
 * @file        graphics/pipeline/scene_effects.h
 * @brief       Modern graphics effects layered onto the guest's own rendering,
 *              shared by the GPU backends
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/graphics/register_file.h>

namespace rex::graphics {

// Modern effects layered onto the guest's own HDR scene, driven by the eDRAM
// resolves (host render targets only), on any GPU backend:
// - Sun shadow map resolves (orthographic projections in the camera
//   constants) are copied with their light matrices.
// - A full-scene depth resolve (typically right after a depth pre-pass) is
//   converted to linear view distance, and the camera is taken from the vertex
//   shader constants (scene_fx_camera_constant).
// - At the resolves of the float HDR color render target of the same size,
//   the effects are computed once per frame and composited into the host
//   render target, so the guest's own post-processing (bloom, exposure,
//   tonemapping) and UI come on top - on SDR and HDR displays alike.
//   Predicated tiling works: each tile is composited at its screen offset.
// - At the final image's resolve, the image effects (sharpening, color
//   grading) are applied, and the debug views are drawn.
//
// The effect logic is here; a backend only provides textures, compute
// dispatches and full-screen draws (the virtual functions below).
class SceneEffects {
 public:
  virtual ~SceneEffects();

  // Call for every guest draw (not resolves) - cheap, only reads registers.
  void OnDraw();
  // Call in an open submission, before the render target cache resolves.
  void OnResolve();

 protected:
  using RenderTarget = RenderTargetCache::RenderTarget;

  // Textures the effects own. Formats and sizes are in kTextureInfos.
  enum class Texture : uint32_t {
    // Full resolution linear view distance.
    kDepth,
    // Half resolution view distance, kViewDepthMipCount levels (MIP 0 texel
    // i: scene pixel 2i).
    kViewDepth,
    // Effects at full or half resolution, ping-ponged by the denoisers.
    kAo0,
    kAo1,
    kGi0,
    kGi1,
    kContactShadows0,
    kContactShadows1,
    kReflections0,
    kReflections1,
    kVolumetric,
    kVolumetricBlur,
    kVolumetricHistory0,
    kVolumetricHistory1,
    // Half resolution scene before the effects, this frame and the previous.
    kSceneColor0,
    kSceneColor1,
    // 1x1 sky color (a = valid), this frame and the previous.
    kSky0,
    kSky1,
    // Sun shadow map copies (their own sizes).
    kShadow0,
    kShadow1,
    // A copy of the final image for the image effects.
    kFinalImage,

    kCount,
  };
  enum class TextureFormat : uint32_t {
    kR32Float,
    kR16Float,
    kRGBA16Float,
  };
  struct TextureInfo {
    TextureFormat format;
    uint32_t mip_levels;
    // Created zeroed (otherwise undefined until written).
    bool zeroed;
  };
  static const TextureInfo kTextureInfos[size_t(Texture::kCount)];

  // Compute pipelines. Each shader's resources, in this order: the sources
  // (t0...), a linear clamping sampler (s0) if any, the extra constant buffer
  // (b1) if any, the targets (u0...) - the Vulkan binding numbers follow the
  // same order.
  enum class Pipeline : uint32_t {
    kDepthCopy,
    kDepthCopyMsaa,
    kShadowCopy,
    kDepthPrefilter,
    kAo,
    kAoGi,
    kBlur,
    kBlurRgba,
    kSkyColor,
    kSkyColorMsaa,
    kColorCapture,
    kColorCaptureMsaa,
    kColorCopy,
    kColorCopyMsaa,
    kContactShadows,
    kReflections,
    kVolumetric,
    kTemporal,

    kCount,
  };
  struct PipelineInfo {
    uint32_t source_count;
    bool sampler;
    bool extra_constants;
    uint32_t target_count;
  };
  static const PipelineInfo kPipelineInfos[size_t(Pipeline::kCount)];

  // Full-screen draws into a guest render target (the composite and image
  // pixel shaders, with up to kDrawSourceCount sources and the extra constant
  // buffer).
  enum class Draw : uint32_t {
    // dest * source.
    kCompositeMultiply,
    // dest + source.
    kCompositeAdd,
    // Replaces dest (debug views).
    kCompositeDebug,
    // Replaces dest (sharpening, color grading).
    kImage,

    kCount,
  };

  // A pass input: an effects texture (all MIP levels), or a guest render
  // target (depth: its depth; color: its float color view).
  struct Source {
    Texture texture = Texture::kCount;
    RenderTarget* render_target = nullptr;

    Source() = default;
    Source(Texture texture) : texture(texture) {}
    Source(RenderTarget* render_target) : render_target(render_target) {}
  };
  struct Target {
    Texture texture;
    uint32_t mip = 0;

    Target(Texture texture, uint32_t mip = 0) : texture(texture), mip(mip) {}
  };
  struct Rect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
  };

  static constexpr uint32_t kMaxSources = 8;
  static constexpr uint32_t kMaxTargets = 4;
  static constexpr uint32_t kDrawSourceCount = 10;
  static constexpr uint32_t kViewDepthMipCount = 4;
  static constexpr uint32_t kMaxShadowCascades = 2;
  // FxConstants: 32 dwords - also the minimum guaranteed Vulkan push constant
  // size.
  static constexpr uint32_t kConstantsSize = 128;

  SceneEffects(RenderTargetCache& render_target_cache, const RegisterFile& register_file);

  // Backend.
  virtual uint64_t GetCurrentFrame() const = 0;
  // (Re)creates the texture if it's smaller than width x height (its old
  // contents are lost). False if failed.
  virtual bool EnsureTexture(Texture texture, uint32_t width, uint32_t height) = 0;
  // Dispatches 8x8-thread groups covering width x height. constants are
  // kConstantsSize bytes; extra_constants (b1) as many as given, if the
  // pipeline takes them.
  virtual bool Dispatch(Pipeline pipeline, std::initializer_list<Source> sources,
                        std::initializer_list<Target> targets, const void* constants,
                        const void* extra_constants, size_t extra_constants_size, uint32_t width,
                        uint32_t height) = 0;
  // Draws a full-screen triangle into the rectangle of a guest color render
  // target, with sources t0... (fewer than kDrawSourceCount: the rest unused).
  virtual bool DrawFullscreen(Draw draw, RenderTarget& render_target, const Rect& rect,
                              std::initializer_list<Source> sources, const void* constants,
                              const void* extra_constants, size_t extra_constants_size) = 0;

 private:
  struct Camera {
    // Projected (clip z / w) depth d to view distance z = proj_b / (d - proj_a).
    float proj_a;
    float proj_b;
    float inv_p00;
    float inv_p11;
    // The world-to-clip rows (c0...c3) the projection was taken from, if any.
    bool has_matrix;
    float rows[4][4];
  };
  // The camera's world transform: world = position + right * x + up * y +
  // forward * z for view space (x, y, z).
  struct CameraBasis {
    float right[3];
    float up[3];
    float forward[3];
    float position[3];
    // The world axis closest to the camera's up vector, and its direction.
    uint32_t up_axis;
    float up_sign;
  };
  // The viewport's depth transform, stored depth = offset + scale * clip z / w
  // (games with float depth often reverse it this way).
  struct DepthRange {
    float scale = 1.0f;
    float offset = 0.0f;
  };
  // A sun shadow map resolved this frame.
  // A sun shadow map, kept across frames - games often update their coarser
  // cascades only every few frames.
  struct ShadowCascade {
    float rows[3][4];
    DepthRange depth_range;
    uint32_t width;
    uint32_t height;
    // World units covered, to tell the cascades apart.
    float coverage;
    uint64_t frame;
    bool valid;
    Texture texture;
  };
  // The camera constants and depth range of the depth-writing draws since the
  // last resolve, counted: the scene's camera is the most common one, not
  // whatever happened to be drawn last (which may be an object with its own
  // transform or depth range).
  struct DrawState {
    float rows[4][4];
    DepthRange depth_range;
    uint32_t count;
  };
  static constexpr uint32_t kMaxDrawStates = 8;
  // Frames a cascade stays usable without being drawn again.
  static constexpr uint64_t kShadowCascadeMaxAge = 8;

  static bool AnySceneEffectEnabled();
  static bool AnyImageEffectEnabled();
  // Whether the sun's shadow maps are used by an enabled effect.
  static bool SunShadowsNeeded();
  void ReadCameraConstants(float rows[4][4]) const;
  static bool IsOrthographic(const float rows[4][4]);
  static bool IsPerspective(const float rows[4][4]);
  static bool GetCameraBasis(const float rows[4][4], CameraBasis& basis_out);
  // The most common draw state (camera constants and depth range) of the pass
  // being resolved, orthographic (a shadow map) or perspective (the scene).
  const DrawState* GetDominantDrawState(bool orthographic) const;
  void CaptureCamera(const float rows[4][4]);
  void CaptureShadowCascade(RenderTarget& depth_rt, const DrawState& draw_state, uint32_t width,
                            uint32_t height, bool trace);
  bool IsShadowCascadeUsable(const ShadowCascade& cascade) const;
  // For the captured scene's stored depth d: view distance z = b / (d - a).
  void GetDepthToDistance(float& a_out, float& b_out) const;
  bool EnsureSceneTextures(uint32_t width, uint32_t height);
  void FillConstants(void* constants_out, int32_t screen_offset_x, int32_t screen_offset_y) const;
  // This frame's view space position to the previous frame's clip space.
  bool GetReprojection(float reproject_out[4][4]) const;
  // The view space direction to world height and the camera's height.
  bool GetHeightRow(const CameraBasis& basis, float height_out[4]) const;
  bool GetSunDirection(const CameraBasis& basis, float toward_sun_out[3]) const;
  static void GetSunTint(float tint_out[3]);
  // The sun, the shadow cascades captured this frame and the effects'
  // parameters (scene_fx_sun.hlsli), and the shadow textures to bind.
  void FillSunConstants(const CameraBasis& basis, void* constants_out,
                        Source shadow_sources_out[kMaxShadowCascades]) const;
  uint32_t GetShadowCascadeCount() const;

  // Once per frame, at the first HDR tile.
  void ComputeEffects(RenderTarget& color_rt, const Rect& rect, int32_t screen_offset_x,
                      int32_t screen_offset_y, bool trace);
  void PrefilterDepth();
  // Into kAo0, and with global illumination into kGi0 (returns whether it
  // was computed).
  bool ComputeAmbientOcclusion(bool global_illumination);
  bool ComputeContactShadows();
  bool ComputeReflections();
  bool ComputeVolumetrics();
  bool FillFogConstants();
  void EstimateSkyColor(RenderTarget& color_rt, const Rect& rect, int32_t screen_offset_x,
                        int32_t screen_offset_y);
  void CaptureSceneColor(RenderTarget& color_rt, const Rect& rect, int32_t screen_offset_x,
                         int32_t screen_offset_y);
  // Separable bilateral denoise, ending in texture_0.
  void Blur(Texture texture_0, Texture texture_1, bool rgba, uint32_t scale);
  void Composite(RenderTarget& color_rt, const Rect& rect, int32_t screen_offset_x,
                 int32_t screen_offset_y, float output_scale_x, float output_scale_y, Draw draw,
                 bool trace);
  void ApplyImageEffects(RenderTarget& color_rt, const Rect& rect, uint32_t width,
                         uint32_t height, bool trace);

  RenderTargetCache& render_target_cache_;
  const RegisterFile& register_file_;

  uint32_t texture_width_ = 0;
  uint32_t texture_height_ = 0;
  uint32_t scene_width_ = 0;
  uint32_t scene_height_ = 0;
  uint64_t depth_frame_ = UINT64_MAX;
  // The last frame with the scene's HDR color resolved, and with the effects.
  uint64_t hdr_frame_ = UINT64_MAX;
  uint64_t effects_frame_ = UINT64_MAX;
  uint64_t image_frame_ = UINT64_MAX;

  // What was computed for the current frame, and at which resolution.
  bool ao_computed_ = false;
  bool gi_computed_ = false;
  bool contact_shadows_computed_ = false;
  bool reflections_computed_ = false;
  bool volumetrics_computed_ = false;
  bool fog_enabled_ = false;
  uint32_t ao_scale_ = 2;
  uint32_t volumetric_scale_ = 2;
  Texture volumetric_result_ = Texture::kVolumetric;
  // The composite's extra constants for this frame (the fog's, and the
  // effects' strengths).
  float composite_constants_[6][4] = {};

  Camera camera_ = {};
  bool camera_logged_ = false;
  // The camera of the previous frame with effects, for reprojection.
  float previous_camera_[4][4] = {};
  bool previous_camera_valid_ = false;
  // Of the depth-writing draws since the last resolve.
  DrawState draw_states_[kMaxDrawStates] = {};
  uint32_t draw_state_count_ = 0;
  // Of the captured scene depth.
  DepthRange scene_depth_range_;
  // The finer cascade first.
  ShadowCascade shadow_cascades_[kMaxShadowCascades] = {};

  // Ping-pong indices (0/1) of this frame's scene color, sky and volumetric
  // history.
  uint32_t scene_color_index_ = 0;
  bool scene_color_valid_[2] = {};
  uint32_t sky_index_ = 0;
  uint32_t volumetric_history_index_ = 0;
  bool volumetric_history_valid_ = false;
  uint32_t volumetric_history_scale_ = 0;

  // The frames scene_fx_trace is logging, [first, end).
  uint64_t trace_frame_ = UINT64_MAX;
  uint64_t trace_frame_end_ = 0;
};

}  // namespace rex::graphics
