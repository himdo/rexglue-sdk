/**
 * @file        graphics/vulkan/scene_effects.h
 * @brief       Vulkan backend of the modern graphics effects
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

#include <rex/graphics/pipeline/scene_effects.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

// Textures, compute dispatches and full-screen draws for SceneEffects (see
// rex/graphics/pipeline/scene_effects.h), with the command processor's
// deferred command buffer and barriers. The effects' own images stay in the
// general layout; draws into the guest's render targets use dynamic rendering
// where available, otherwise a render pass with a one-use framebuffer.
class VulkanSceneEffects final : public SceneEffects {
 public:
  VulkanSceneEffects(VulkanCommandProcessor& command_processor,
                     VulkanRenderTargetCache& render_target_cache,
                     const RegisterFile& register_file);
  ~VulkanSceneEffects() override;

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
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    // All MIP levels.
    VkImageView sampled_view = VK_NULL_HANDLE;
    VkImageView storage_views[kViewDepthMipCount] = {};
    uint32_t width = 0;
    uint32_t height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // The last write (or clear).
    VkPipelineStageFlags write_stage_mask = 0;
    VkAccessFlags write_access_mask = 0;
    // The reads since, which the last write has been made visible to.
    VkPipelineStageFlags read_stage_mask = 0;
    VkAccessFlags read_access_mask = 0;
  };
  struct ComputePipeline {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
  };
  // The composite and the image pixel shaders' resources.
  enum class DrawLayout : uint32_t {
    kComposite,
    kImage,

    kCount,
  };
  // Destroyed once the GPU has finished the submission.
  struct Garbage {
    uint64_t submission;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView views[1 + kViewDepthMipCount];
    VkFramebuffer framebuffer;
  };
  struct DescriptorPool {
    VkDescriptorPool pool;
    uint64_t last_submission;
  };

  static VkFormat GetFormat(TextureFormat format);
  static DrawLayout GetDrawLayout(Draw draw) {
    return draw == Draw::kImage ? DrawLayout::kImage : DrawLayout::kComposite;
  }
  uint32_t GetHostSampleCount(const RenderTarget& render_target) const;
  // Barriers for this use of an effects texture (all in the general layout).
  void UseTexture(Texture texture, VkPipelineStageFlags stage_mask, VkAccessFlags access_mask);
  void UseRenderTarget(RenderTarget& render_target, VkPipelineStageFlags stage_mask,
                       VkAccessFlags access_mask, VkImageLayout layout);
  VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout layout);
  // The descriptor for reading a source in the layout it's being used in.
  VkDescriptorImageInfo GetSourceInfo(const Source& source) const;
  bool UploadConstants(const void* data, size_t size, VkDescriptorBufferInfo& info_out);
  VkPipeline GetDrawPipeline(Draw draw, VkFormat format, uint32_t samples);
  VkRenderPass GetRenderPass(VkFormat format, uint32_t samples);
  void DestroyTexture(TextureResource& texture, bool immediately);

  VulkanCommandProcessor& command_processor_;
  VulkanRenderTargetCache& vulkan_render_target_cache_;

  VkSampler sampler_ = VK_NULL_HANDLE;
  ComputePipeline compute_pipelines_[size_t(Pipeline::kCount)];
  VkDescriptorSetLayout draw_set_layouts_[size_t(DrawLayout::kCount)] = {};
  VkPipelineLayout draw_pipeline_layouts_[size_t(DrawLayout::kCount)] = {};
  VkShaderModule fullscreen_vs_ = VK_NULL_HANDLE;
  VkShaderModule draw_ps_[size_t(DrawLayout::kCount)] = {};
  std::map<std::tuple<Draw, VkFormat, uint32_t>, VkPipeline> draw_pipelines_;
  // Without dynamic rendering.
  std::map<std::pair<VkFormat, uint32_t>, VkRenderPass> render_passes_;
  bool dynamic_rendering_ = false;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_pool_;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  uint64_t descriptor_pool_submission_ = 0;
  std::vector<DescriptorPool> descriptor_pools_in_flight_;
  std::vector<VkDescriptorPool> descriptor_pools_free_;

  TextureResource textures_[size_t(Texture::kCount)];
  std::vector<Garbage> garbage_;
};

}  // namespace rex::graphics::vulkan
