// Copies the guest's depth (sample 0 of a host depth render target):
// - Default: the scene depth as linear view distance over the whole scene.
// - FX_RAW: a shadow map's stored depth as is (orthographic, already linear).
// FX_MSAA selects the multisampled source variant.

#include "scene_fx_common.hlsli"

#ifdef FX_MSAA
FX_BINDING(0) Texture2DMS<float> fx_source : register(t0);
#else
FX_BINDING(0) Texture2D<float> fx_source : register(t0);
#endif
FX_BINDING(1) FX_FORMAT_R32F RWTexture2D<float> fx_depth : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= fx_scene_size)) {
    return;
  }
  int2 source = int2(id.xy) - fx_screen_offset;
#ifdef FX_MSAA
  float device_depth = fx_source.Load(source, 0);
#else
  float device_depth = fx_source.Load(int3(source, 0));
#endif
#ifdef FX_RAW
  fx_depth[id.xy] = device_depth;
#else
  float distance = FxViewDistance(device_depth);
  // Past the far plane (or numerically behind the camera): treat as sky.
  fx_depth[id.xy] = distance > 0.0 ? min(distance, 1.0e6) : 1.0e6;
#endif
}
