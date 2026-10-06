/**
 * @file        graphics/d3d12/scene_effects.h
 * @brief       Direct3D 12 backend of the modern graphics effects
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/pipeline/scene_effects.h>
#include <rex/graphics/register_file.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_upload_buffer_pool.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;

// Textures, compute dispatches and full-screen draws for SceneEffects (see
// rex/graphics/pipeline/scene_effects.h), with the command processor's
// deferred command list, barriers and one-use descriptors.
class D3D12SceneEffects final : public SceneEffects {
 public:
  D3D12SceneEffects(D3D12CommandProcessor& command_processor,
                    D3D12RenderTargetCache& render_target_cache, const RegisterFile& register_file);
  ~D3D12SceneEffects() override;

  bool Initialize();
  void Shutdown();

  // Releases what the GPU has finished with - call before OnResolve.
  void ReleaseCompletedResources();

 protected:
  uint64_t GetCurrentFrame() const override;
  bool EnsureTexture(Texture texture, uint32_t width, uint32_t height) override;
  bool Dispatch(Pipeline pipeline, std::initializer_list<Source> sources,
                std::initializer_list<Target> targets, const void* constants,
                const void* extra_constants, size_t extra_constants_size, uint32_t width,
                uint32_t height) override;
  bool DrawFullscreen(Draw draw, RenderTarget& render_target, const Rect& rect,
                      std::initializer_list<Source> sources, const void* constants,
                      const void* extra_constants, size_t extra_constants_size) override;

 private:
  struct TextureResource {
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  static DXGI_FORMAT GetFormat(TextureFormat format);
  void TransitionTexture(Texture texture, D3D12_RESOURCE_STATES new_state);
  void TransitionRenderTarget(RenderTarget& render_target, D3D12_RESOURCE_STATES new_state);
  // Transitions the source for reading and writes its view.
  void WriteSourceView(D3D12_CPU_DESCRIPTOR_HANDLE handle, const Source& source,
                       D3D12_RESOURCE_STATES read_state);
  D3D12_GPU_VIRTUAL_ADDRESS UploadConstants(const void* data, size_t size);
  uint32_t GetHostSampleCount(const RenderTarget& render_target) const;
  ID3D12PipelineState* GetDrawPipeline(Draw draw, DXGI_FORMAT format, uint32_t samples);
  void ReleaseTexture(TextureResource& texture, bool immediately);

  D3D12CommandProcessor& command_processor_;
  D3D12RenderTargetCache& d3d12_render_target_cache_;

  // 0: 32 root constants (b0); 1...8: SRV tables (t0...t7); 9...12: UAV tables
  // (u0...u3) - single descriptors, as one-use descriptors aren't contiguous
  // with bindless resources; 13: root CBV (b1); static linear clamping
  // sampler (s0).
  ID3D12RootSignature* compute_root_signature_ = nullptr;
  // 0: 32 root constants (b0); 1...kDrawSourceCount: SRV tables; then a root
  // CBV (b1); static linear clamping sampler (s0).
  ID3D12RootSignature* draw_root_signature_ = nullptr;
  ID3D12PipelineState* pipelines_[size_t(Pipeline::kCount)] = {};
  std::map<std::tuple<Draw, DXGI_FORMAT, uint32_t>, ID3D12PipelineState*> draw_pipelines_;
  std::unique_ptr<ui::d3d12::D3D12UploadBufferPool> upload_pool_;

  TextureResource textures_[size_t(Texture::kCount)];
  // Replaced textures, released once the GPU has finished the submission.
  std::vector<std::pair<uint64_t, ID3D12Resource*>> resources_to_release_;
};

}  // namespace rex::graphics::d3d12
