/**
 * @file        graphics/vulkan/scene_effects.cpp
 * @brief       Vulkan backend of the modern graphics effects
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/vulkan/scene_effects.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DECLARE(bool, vulkan_dynamic_rendering);

namespace rex::graphics::vulkan {

namespace shaders {
#include "../shaders/vulkan_spirv/scene_fx_ao_blur_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_ao_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_ao_gi_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_ao_prefilter_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_blur_rgba_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_color_capture_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_color_capture_msaa_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_color_copy_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_color_copy_msaa_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_composite_ps.h"
#include "../shaders/vulkan_spirv/scene_fx_contact_shadows_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_depth_copy_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_depth_copy_msaa_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_fullscreen_vs.h"
#include "../shaders/vulkan_spirv/scene_fx_image_ps.h"
#include "../shaders/vulkan_spirv/scene_fx_reflections_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_shadow_copy_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_sky_color_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_sky_color_msaa_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_temporal_cs.h"
#include "../shaders/vulkan_spirv/scene_fx_volumetric_cs.h"
}  // namespace shaders

namespace {

struct ShaderCode {
  const uint32_t* code;
  size_t size;
};
#define REX_SCENE_FX_SHADER(name) {shaders::name, sizeof(shaders::name)}
// In the order of SceneEffects::Pipeline.
const ShaderCode kComputeShaders[] = {
    REX_SCENE_FX_SHADER(scene_fx_depth_copy_cs),
    REX_SCENE_FX_SHADER(scene_fx_depth_copy_msaa_cs),
    REX_SCENE_FX_SHADER(scene_fx_shadow_copy_cs),
    REX_SCENE_FX_SHADER(scene_fx_ao_prefilter_cs),
    REX_SCENE_FX_SHADER(scene_fx_ao_cs),
    REX_SCENE_FX_SHADER(scene_fx_ao_gi_cs),
    REX_SCENE_FX_SHADER(scene_fx_ao_blur_cs),
    REX_SCENE_FX_SHADER(scene_fx_blur_rgba_cs),
    REX_SCENE_FX_SHADER(scene_fx_sky_color_cs),
    REX_SCENE_FX_SHADER(scene_fx_sky_color_msaa_cs),
    REX_SCENE_FX_SHADER(scene_fx_color_capture_cs),
    REX_SCENE_FX_SHADER(scene_fx_color_capture_msaa_cs),
    REX_SCENE_FX_SHADER(scene_fx_color_copy_cs),
    REX_SCENE_FX_SHADER(scene_fx_color_copy_msaa_cs),
    REX_SCENE_FX_SHADER(scene_fx_contact_shadows_cs),
    REX_SCENE_FX_SHADER(scene_fx_reflections_cs),
    REX_SCENE_FX_SHADER(scene_fx_volumetric_cs),
    REX_SCENE_FX_SHADER(scene_fx_temporal_cs),
};
#undef REX_SCENE_FX_SHADER

// Sources of the draws' pixel shaders, then their constant buffer.
constexpr uint32_t kCompositeSourceCount = 9;
constexpr uint32_t kImageSourceCount = 1;
// Descriptor sets per pool.
constexpr uint32_t kDescriptorPoolSets = 128;

}  // namespace

VulkanSceneEffects::VulkanSceneEffects(VulkanCommandProcessor& command_processor,
                                       VulkanRenderTargetCache& render_target_cache,
                                       const RegisterFile& register_file)
    : SceneEffects(render_target_cache, register_file),
      command_processor_(command_processor),
      vulkan_render_target_cache_(render_target_cache) {
  static_assert(std::size(kComputeShaders) == size_t(Pipeline::kCount));
}

VulkanSceneEffects::~VulkanSceneEffects() { Shutdown(); }

bool VulkanSceneEffects::Initialize() {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  // The texture formats must be usable as storage images.
  const ui::vulkan::VulkanInstance::Functions& ifn = vulkan_device->vulkan_instance()->functions();
  for (TextureFormat format :
       {TextureFormat::kR32Float, TextureFormat::kR16Float, TextureFormat::kRGBA16Float}) {
    VkFormatProperties properties;
    ifn.vkGetPhysicalDeviceFormatProperties(vulkan_device->physical_device(), GetFormat(format),
                                            &properties);
    constexpr VkFormatFeatureFlags kNeeded =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & kNeeded) != kNeeded) {
      REXGPU_WARN("Scene effects: format {} unsupported for storage, effects unavailable",
                  uint32_t(GetFormat(format)));
      return false;
    }
  }
  dynamic_rendering_ =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;

  VkSamplerCreateInfo sampler_info = {};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  if (dfn.vkCreateSampler(device, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
    REXGPU_ERROR("Scene effects: failed to create the sampler");
    return false;
  }

  // Compute: per pipeline, its bindings in the order of the shaders'
  // resources (sources, sampler, constant buffer, targets) and 128 bytes of
  // push constants.
  for (uint32_t i = 0; i < uint32_t(Pipeline::kCount); ++i) {
    const PipelineInfo& info = kPipelineInfos[i];
    VkDescriptorSetLayoutBinding bindings[kMaxSources + 2 + kMaxTargets];
    uint32_t binding_count = 0;
    auto add_binding = [&](VkDescriptorType type, const VkSampler* immutable_sampler) {
      VkDescriptorSetLayoutBinding& binding = bindings[binding_count];
      binding.binding = binding_count++;
      binding.descriptorType = type;
      binding.descriptorCount = 1;
      binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      binding.pImmutableSamplers = immutable_sampler;
    };
    for (uint32_t j = 0; j < info.source_count; ++j) {
      add_binding(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr);
    }
    if (info.sampler) {
      add_binding(VK_DESCRIPTOR_TYPE_SAMPLER, &sampler_);
    }
    if (info.extra_constants) {
      add_binding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr);
    }
    for (uint32_t j = 0; j < info.target_count; ++j) {
      add_binding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nullptr);
    }
    ComputePipeline& pipeline = compute_pipelines_[i];
    VkDescriptorSetLayoutCreateInfo set_layout_info = {};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = binding_count;
    set_layout_info.pBindings = bindings;
    VkPushConstantRange push_constants = {VK_SHADER_STAGE_COMPUTE_BIT, 0, kConstantsSize};
    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &pipeline.set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constants;
    if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                        &pipeline.set_layout) != VK_SUCCESS ||
        dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline.layout) !=
            VK_SUCCESS) {
      REXGPU_ERROR("Scene effects: failed to create compute pipeline layout {}", i);
      return false;
    }
    pipeline.pipeline = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, pipeline.layout, kComputeShaders[i].code, kComputeShaders[i].size);
    if (pipeline.pipeline == VK_NULL_HANDLE) {
      REXGPU_ERROR("Scene effects: failed to create compute pipeline {}", i);
      return false;
    }
  }

  // Draws: the pixel shader's sources, its constant buffer, push constants.
  for (uint32_t i = 0; i < uint32_t(DrawLayout::kCount); ++i) {
    uint32_t source_count =
        DrawLayout(i) == DrawLayout::kImage ? kImageSourceCount : kCompositeSourceCount;
    VkDescriptorSetLayoutBinding bindings[kDrawSourceCount + 1];
    for (uint32_t j = 0; j <= source_count; ++j) {
      VkDescriptorSetLayoutBinding& binding = bindings[j];
      binding.binding = j;
      binding.descriptorType = j < source_count ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                                : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      binding.descriptorCount = 1;
      binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      binding.pImmutableSamplers = nullptr;
    }
    VkDescriptorSetLayoutCreateInfo set_layout_info = {};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.bindingCount = source_count + 1;
    set_layout_info.pBindings = bindings;
    VkPushConstantRange push_constants = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, kConstantsSize};
    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &draw_set_layouts_[i];
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constants;
    if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr,
                                        &draw_set_layouts_[i]) != VK_SUCCESS ||
        dfn.vkCreatePipelineLayout(device, &layout_info, nullptr, &draw_pipeline_layouts_[i]) !=
            VK_SUCCESS) {
      REXGPU_ERROR("Scene effects: failed to create draw pipeline layout {}", i);
      return false;
    }
  }
  fullscreen_vs_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::scene_fx_fullscreen_vs, sizeof(shaders::scene_fx_fullscreen_vs));
  draw_ps_[size_t(DrawLayout::kComposite)] = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::scene_fx_composite_ps, sizeof(shaders::scene_fx_composite_ps));
  draw_ps_[size_t(DrawLayout::kImage)] = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::scene_fx_image_ps, sizeof(shaders::scene_fx_image_ps));
  if (fullscreen_vs_ == VK_NULL_HANDLE || draw_ps_[0] == VK_NULL_HANDLE ||
      draw_ps_[1] == VK_NULL_HANDLE) {
    REXGPU_ERROR("Scene effects: failed to create the draw shader modules");
    return false;
  }

  upload_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 64 * 1024);
  return true;
}

void VulkanSceneEffects::Shutdown() {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  if (!vulkan_device) {
    return;
  }
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();
  // The command processor has awaited the GPU before shutting down.
  for (TextureResource& texture : textures_) {
    DestroyTexture(texture, true);
  }
  for (Garbage& garbage : garbage_) {
    for (VkImageView view : garbage.views) {
      if (view != VK_NULL_HANDLE) {
        dfn.vkDestroyImageView(device, view, nullptr);
      }
    }
    if (garbage.image != VK_NULL_HANDLE) {
      dfn.vkDestroyImage(device, garbage.image, nullptr);
    }
    if (garbage.memory != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, garbage.memory, nullptr);
    }
    if (garbage.framebuffer != VK_NULL_HANDLE) {
      dfn.vkDestroyFramebuffer(device, garbage.framebuffer, nullptr);
    }
  }
  garbage_.clear();
  upload_pool_.reset();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, descriptor_pool_);
  for (DescriptorPool& pool : descriptor_pools_in_flight_) {
    dfn.vkDestroyDescriptorPool(device, pool.pool, nullptr);
  }
  descriptor_pools_in_flight_.clear();
  for (VkDescriptorPool pool : descriptor_pools_free_) {
    dfn.vkDestroyDescriptorPool(device, pool, nullptr);
  }
  descriptor_pools_free_.clear();
  for (auto& pipeline : draw_pipelines_) {
    if (pipeline.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, pipeline.second, nullptr);
    }
  }
  draw_pipelines_.clear();
  for (auto& render_pass : render_passes_) {
    if (render_pass.second != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass.second, nullptr);
    }
  }
  render_passes_.clear();
  for (VkShaderModule& module : draw_ps_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, module);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, fullscreen_vs_);
  for (uint32_t i = 0; i < uint32_t(DrawLayout::kCount); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                           draw_pipeline_layouts_[i]);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                           draw_set_layouts_[i]);
  }
  for (ComputePipeline& pipeline : compute_pipelines_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, pipeline.pipeline);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, pipeline.layout);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                           pipeline.set_layout);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device, sampler_);
}

void VulkanSceneEffects::ReleaseCompletedResources() {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();
  uint64_t completed = command_processor_.GetCompletedSubmission();
  std::erase_if(garbage_, [&](const Garbage& garbage) {
    if (garbage.submission > completed) {
      return false;
    }
    for (VkImageView view : garbage.views) {
      if (view != VK_NULL_HANDLE) {
        dfn.vkDestroyImageView(device, view, nullptr);
      }
    }
    if (garbage.image != VK_NULL_HANDLE) {
      dfn.vkDestroyImage(device, garbage.image, nullptr);
    }
    if (garbage.memory != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, garbage.memory, nullptr);
    }
    if (garbage.framebuffer != VK_NULL_HANDLE) {
      dfn.vkDestroyFramebuffer(device, garbage.framebuffer, nullptr);
    }
    return true;
  });
  std::erase_if(descriptor_pools_in_flight_, [&](const DescriptorPool& pool) {
    if (pool.last_submission > completed) {
      return false;
    }
    dfn.vkResetDescriptorPool(device, pool.pool, 0);
    descriptor_pools_free_.push_back(pool.pool);
    return true;
  });
  if (upload_pool_) {
    upload_pool_->Reclaim(completed);
  }
}

uint64_t VulkanSceneEffects::GetCurrentFrame() const {
  return command_processor_.GetCurrentFrame();
}

VkFormat VulkanSceneEffects::GetFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::kR32Float:
      return VK_FORMAT_R32_SFLOAT;
    case TextureFormat::kR16Float:
      return VK_FORMAT_R16_SFLOAT;
    case TextureFormat::kRGBA16Float:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
  return VK_FORMAT_UNDEFINED;
}

uint32_t VulkanSceneEffects::GetHostSampleCount(const RenderTarget& render_target) const {
  xenos::MsaaSamples samples = render_target.key().msaa_samples;
  // 2x is emulated with 4x where the host lacks it.
  if (samples == xenos::MsaaSamples::k2X &&
      !vulkan_render_target_cache_.IsMsaa2xSupported(true)) {
    return 4;
  }
  return 1u << uint32_t(samples);
}

void VulkanSceneEffects::DestroyTexture(TextureResource& texture, bool immediately) {
  if (texture.image == VK_NULL_HANDLE) {
    return;
  }
  Garbage garbage = {};
  garbage.submission = command_processor_.GetCurrentSubmission();
  garbage.image = texture.image;
  garbage.memory = texture.memory;
  garbage.views[0] = texture.sampled_view;
  for (uint32_t i = 0; i < kViewDepthMipCount; ++i) {
    garbage.views[1 + i] = texture.storage_views[i];
  }
  if (immediately) {
    const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    VkDevice device = vulkan_device->device();
    for (VkImageView view : garbage.views) {
      if (view != VK_NULL_HANDLE) {
        dfn.vkDestroyImageView(device, view, nullptr);
      }
    }
    dfn.vkDestroyImage(device, garbage.image, nullptr);
    dfn.vkFreeMemory(device, garbage.memory, nullptr);
  } else {
    garbage_.push_back(garbage);
  }
  uint32_t width = texture.width, height = texture.height;
  texture = TextureResource();
  // Keep the size, so a replacement only ever grows.
  texture.width = width;
  texture.height = height;
}

bool VulkanSceneEffects::EnsureTexture(Texture texture, uint32_t width, uint32_t height) {
  TextureResource& resource = textures_[size_t(texture)];
  if (resource.image != VK_NULL_HANDLE && resource.width >= width &&
      resource.height >= height) {
    return true;
  }
  DestroyTexture(resource, false);
  const TextureInfo& info = kTextureInfos[size_t(texture)];
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();
  VkFormat format = GetFormat(info.format);
  VkImageCreateInfo image_info = {};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent.width = std::max(width, resource.width);
  image_info.extent.height = std::max(height, resource.height);
  image_info.extent.depth = 1;
  image_info.mipLevels = info.mip_levels;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage =
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          resource.image, resource.memory)) {
    resource.image = VK_NULL_HANDLE;
    return false;
  }
  VkImageViewCreateInfo view_info = {};
  view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image = resource.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  bool views_created =
      dfn.vkCreateImageView(device, &view_info, nullptr, &resource.sampled_view) == VK_SUCCESS;
  for (uint32_t mip = 0; mip < info.mip_levels && views_created; ++mip) {
    view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange(
        VK_IMAGE_ASPECT_COLOR_BIT, mip, 1);
    views_created = dfn.vkCreateImageView(device, &view_info, nullptr,
                                          &resource.storage_views[mip]) == VK_SUCCESS;
  }
  resource.width = image_info.extent.width;
  resource.height = image_info.extent.height;
  if (!views_created) {
    DestroyTexture(resource, true);
    return false;
  }
  if (info.zeroed) {
    // Cleared to zero before its first use.
    command_processor_.EndRenderPass();
    command_processor_.PushImageMemoryBarrier(
        resource.image, ui::vulkan::util::InitializeSubresourceRange(), 0,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL);
    command_processor_.SubmitBarriers(true);
    VkClearColorValue zero = {};
    VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
    command_processor_.deferred_command_buffer().CmdVkClearColorImage(
        resource.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    resource.layout = VK_IMAGE_LAYOUT_GENERAL;
    resource.write_stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    resource.write_access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
  }
  return true;
}

void VulkanSceneEffects::UseTexture(Texture texture, VkPipelineStageFlags stage_mask,
                                    VkAccessFlags access_mask) {
  TextureResource& resource = textures_[size_t(texture)];
  bool write = (access_mask & VK_ACCESS_SHADER_WRITE_BIT) != 0;
  if (write) {
    // After the last write and the reads since.
    command_processor_.PushImageMemoryBarrier(
        resource.image, ui::vulkan::util::InitializeSubresourceRange(),
        resource.write_stage_mask | resource.read_stage_mask, stage_mask,
        resource.write_access_mask, access_mask, resource.layout, VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
    resource.write_stage_mask = stage_mask;
    resource.write_access_mask = access_mask;
    resource.read_stage_mask = 0;
    resource.read_access_mask = 0;
  } else if (resource.layout != VK_IMAGE_LAYOUT_GENERAL ||
             (stage_mask & ~resource.read_stage_mask) ||
             (access_mask & ~resource.read_access_mask)) {
    // The last write must be made visible to this stage - unless an earlier
    // barrier already has.
    command_processor_.PushImageMemoryBarrier(
        resource.image, ui::vulkan::util::InitializeSubresourceRange(),
        resource.write_stage_mask, stage_mask, resource.write_access_mask, access_mask,
        resource.layout, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, false);
    if (resource.layout != VK_IMAGE_LAYOUT_GENERAL) {
      // Later uses must come after the layout transition, which is before
      // this stage.
      resource.write_stage_mask = stage_mask;
      resource.write_access_mask = 0;
      resource.read_stage_mask = stage_mask;
      resource.read_access_mask = access_mask;
    } else {
      resource.read_stage_mask |= stage_mask;
      resource.read_access_mask |= access_mask;
    }
  }
  resource.layout = VK_IMAGE_LAYOUT_GENERAL;
}

void VulkanSceneEffects::UseRenderTarget(RenderTarget& render_target,
                                         VkPipelineStageFlags stage_mask,
                                         VkAccessFlags access_mask, VkImageLayout layout) {
  auto& vulkan_rt = static_cast<VulkanRenderTargetCache::VulkanRenderTarget&>(render_target);
  VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange(
      vulkan_rt.key().is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                               : VK_IMAGE_ASPECT_COLOR_BIT);
  // Not skipped even if the usage is the same: the guest's draws and the
  // effects' are in different render pass instances.
  command_processor_.PushImageMemoryBarrier(
      vulkan_rt.image(), range, vulkan_rt.current_stage_mask(), stage_mask,
      vulkan_rt.current_access_mask(), access_mask, vulkan_rt.current_layout(), layout,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  vulkan_rt.SetUsage(stage_mask, access_mask, layout);
}

VkDescriptorImageInfo VulkanSceneEffects::GetSourceInfo(const Source& source) const {
  VkDescriptorImageInfo info = {};
  if (source.render_target) {
    // For depth, the depth aspect alone; for color, the float drawing view.
    auto& vulkan_rt =
        static_cast<const VulkanRenderTargetCache::VulkanRenderTarget&>(*source.render_target);
    info.imageView = vulkan_rt.view_depth_color();
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  } else {
    info.imageView = textures_[size_t(source.texture)].sampled_view;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  }
  return info;
}

VkDescriptorSet VulkanSceneEffects::AllocateDescriptorSet(VkDescriptorSetLayout layout) {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();
  uint64_t submission = command_processor_.GetCurrentSubmission();
  for (uint32_t attempt = 0; attempt < 2; ++attempt) {
    if (descriptor_pool_ == VK_NULL_HANDLE) {
      if (!descriptor_pools_free_.empty()) {
        descriptor_pool_ = descriptor_pools_free_.back();
        descriptor_pools_free_.pop_back();
      } else {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDescriptorPoolSets * kDrawSourceCount},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kDescriptorPoolSets * kMaxTargets},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDescriptorPoolSets},
            {VK_DESCRIPTOR_TYPE_SAMPLER, kDescriptorPoolSets},
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = kDescriptorPoolSets;
        pool_info.poolSizeCount = uint32_t(std::size(sizes));
        pool_info.pPoolSizes = sizes;
        if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool_) !=
            VK_SUCCESS) {
          descriptor_pool_ = VK_NULL_HANDLE;
          return VK_NULL_HANDLE;
        }
      }
    }
    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = descriptor_pool_;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &layout;
    VkDescriptorSet set;
    if (dfn.vkAllocateDescriptorSets(device, &allocate_info, &set) == VK_SUCCESS) {
      descriptor_pool_submission_ = submission;
      return set;
    }
    // Full: retire it until the GPU is done with it, and take another.
    descriptor_pools_in_flight_.push_back({descriptor_pool_, descriptor_pool_submission_});
    descriptor_pool_ = VK_NULL_HANDLE;
  }
  return VK_NULL_HANDLE;
}

bool VulkanSceneEffects::UploadConstants(const void* data, size_t size,
                                         VkDescriptorBufferInfo& info_out) {
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  VkBuffer buffer;
  VkDeviceSize offset;
  uint8_t* mapping = upload_pool_->Request(
      command_processor_.GetCurrentSubmission(), size,
      size_t(std::max(vulkan_device->properties().minUniformBufferOffsetAlignment,
                      VkDeviceSize(16))),
      buffer, offset);
  if (!mapping) {
    return false;
  }
  std::memcpy(mapping, data, size);
  upload_pool_->FlushWrites();
  info_out.buffer = buffer;
  info_out.offset = offset;
  info_out.range = size;
  return true;
}

bool VulkanSceneEffects::Dispatch(Pipeline pipeline, std::initializer_list<Source> sources,
                                  std::initializer_list<Target> targets, const void* constants,
                                  const void* extra_constants, size_t extra_constants_size,
                                  uint32_t width, uint32_t height) {
  const PipelineInfo& info = kPipelineInfos[size_t(pipeline)];
  assert_true(sources.size() == info.source_count && targets.size() == info.target_count);
  const ComputePipeline& compute_pipeline = compute_pipelines_[size_t(pipeline)];
  VkDescriptorBufferInfo buffer_info = {};
  if (info.extra_constants && !UploadConstants(extra_constants, extra_constants_size, buffer_info)) {
    return false;
  }
  VkDescriptorSet set = AllocateDescriptorSet(compute_pipeline.set_layout);
  if (set == VK_NULL_HANDLE) {
    return false;
  }

  command_processor_.EndRenderPass();
  for (const Source& source : sources) {
    if (source.render_target) {
      UseRenderTarget(*source.render_target, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
      UseTexture(source.texture, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
  }
  Texture last_target = Texture::kCount;
  for (const Target& target : targets) {
    // Several MIP levels of one texture are one use.
    if (target.texture != last_target) {
      UseTexture(target.texture, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_WRITE_BIT);
      last_target = target.texture;
    }
  }

  VkDescriptorImageInfo image_infos[kMaxSources + kMaxTargets];
  VkWriteDescriptorSet writes[kMaxSources + 1 + kMaxTargets];
  uint32_t write_count = 0, image_info_count = 0, binding = 0;
  auto add_write = [&](VkDescriptorType type) -> VkWriteDescriptorSet& {
    VkWriteDescriptorSet& write = writes[write_count++];
    write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding++;
    write.descriptorCount = 1;
    write.descriptorType = type;
    return write;
  };
  for (const Source& source : sources) {
    image_infos[image_info_count] = GetSourceInfo(source);
    add_write(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE).pImageInfo = &image_infos[image_info_count++];
  }
  if (info.sampler) {
    // Immutable.
    ++binding;
  }
  if (info.extra_constants) {
    add_write(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER).pBufferInfo = &buffer_info;
  }
  for (const Target& target : targets) {
    VkDescriptorImageInfo& image_info = image_infos[image_info_count++];
    image_info = {};
    image_info.imageView = textures_[size_t(target.texture)].storage_views[target.mip];
    image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    add_write(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE).pImageInfo = &image_info;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  vulkan_device->functions().vkUpdateDescriptorSets(vulkan_device->device(), write_count, writes,
                                                    0, nullptr);

  command_processor_.SubmitBarriers(true);
  command_processor_.BindExternalComputePipeline(compute_pipeline.pipeline);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline.layout,
                                         0, 1, &set, 0, nullptr);
  command_buffer.CmdVkPushConstants(compute_pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    kConstantsSize, constants);
  command_buffer.CmdVkDispatch((width + 7) / 8, (height + 7) / 8, 1);
  return true;
}

bool VulkanSceneEffects::DrawFullscreen(Draw draw, RenderTarget& render_target, const Rect& rect,
                                        std::initializer_list<Source> sources,
                                        const void* constants, const void* extra_constants,
                                        size_t extra_constants_size) {
  auto& vulkan_rt = static_cast<VulkanRenderTargetCache::VulkanRenderTarget&>(render_target);
  DrawLayout draw_layout = GetDrawLayout(draw);
  uint32_t source_count =
      draw_layout == DrawLayout::kImage ? kImageSourceCount : kCompositeSourceCount;
  assert_true(sources.size() == source_count);
  VkFormat format =
      vulkan_render_target_cache_.GetColorVulkanFormat(vulkan_rt.key().GetColorFormat());
  uint32_t samples = GetHostSampleCount(vulkan_rt);
  VkPipeline pipeline = GetDrawPipeline(draw, format, samples);
  if (pipeline == VK_NULL_HANDLE) {
    return false;
  }
  VkDescriptorBufferInfo buffer_info = {};
  if (!UploadConstants(extra_constants, extra_constants_size, buffer_info)) {
    return false;
  }
  VkDescriptorSet set = AllocateDescriptorSet(draw_set_layouts_[size_t(draw_layout)]);
  if (set == VK_NULL_HANDLE) {
    return false;
  }
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (!dynamic_rendering_) {
    render_pass = GetRenderPass(format, samples);
    VkImageView attachment = vulkan_rt.view_depth_color();
    VkFramebufferCreateInfo framebuffer_info = {};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &attachment;
    framebuffer_info.width = uint32_t(rect.right);
    framebuffer_info.height = uint32_t(rect.bottom);
    framebuffer_info.layers = 1;
    if (render_pass == VK_NULL_HANDLE ||
        dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer) != VK_SUCCESS) {
      return false;
    }
    Garbage garbage = {};
    garbage.submission = command_processor_.GetCurrentSubmission();
    garbage.framebuffer = framebuffer;
    garbage_.push_back(garbage);
  }

  command_processor_.EndRenderPass();
  for (const Source& source : sources) {
    UseTexture(source.texture, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
  }
  UseRenderTarget(render_target, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  VkDescriptorImageInfo image_infos[kDrawSourceCount];
  VkWriteDescriptorSet writes[kDrawSourceCount + 1];
  uint32_t write_count = 0;
  for (const Source& source : sources) {
    image_infos[write_count] = GetSourceInfo(source);
    VkWriteDescriptorSet& write = writes[write_count];
    write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = write_count;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image_infos[write_count];
    ++write_count;
  }
  VkWriteDescriptorSet& buffer_write = writes[write_count];
  buffer_write = {};
  buffer_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  buffer_write.dstSet = set;
  buffer_write.dstBinding = write_count++;
  buffer_write.descriptorCount = 1;
  buffer_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  buffer_write.pBufferInfo = &buffer_info;
  dfn.vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);

  command_processor_.SubmitBarriers(true);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  VkRect2D area;
  area.offset.x = rect.left;
  area.offset.y = rect.top;
  area.extent.width = uint32_t(rect.right - rect.left);
  area.extent.height = uint32_t(rect.bottom - rect.top);
  if (dynamic_rendering_) {
    VkRenderingAttachmentInfo attachment = {};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = vulkan_rt.view_depth_color();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.resolveMode = VK_RESOLVE_MODE_NONE;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea = area;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachments = &attachment;
    command_buffer.CmdVkBeginRendering(&rendering_info);
  } else {
    VkRenderPassBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = render_pass;
    begin_info.framebuffer = framebuffer;
    begin_info.renderArea = area;
    command_buffer.CmdVkBeginRenderPass(&begin_info, VK_SUBPASS_CONTENTS_INLINE);
  }
  command_processor_.BindExternalGraphicsPipeline(pipeline);
  VkViewport viewport = {float(rect.left),
                         float(rect.top),
                         float(rect.right - rect.left),
                         float(rect.bottom - rect.top),
                         0.0f,
                         1.0f};
  command_processor_.SetViewport(viewport);
  command_processor_.SetScissor(area);
  VkPipelineLayout layout = draw_pipeline_layouts_[size_t(draw_layout)];
  command_buffer.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                                         nullptr);
  command_buffer.CmdVkPushConstants(layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, kConstantsSize,
                                    constants);
  command_buffer.CmdVkDraw(3, 1, 0, 0);
  if (dynamic_rendering_) {
    command_buffer.CmdVkEndRendering();
  } else {
    command_buffer.CmdVkEndRenderPass();
  }
  return true;
}

VkRenderPass VulkanSceneEffects::GetRenderPass(VkFormat format, uint32_t samples) {
  auto key = std::make_pair(format, samples);
  auto it = render_passes_.find(key);
  if (it != render_passes_.end()) {
    return it->second;
  }
  VkAttachmentDescription attachment = {};
  attachment.format = format;
  attachment.samples = VkSampleCountFlagBits(samples);
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass = {};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &reference;
  VkRenderPassCreateInfo render_pass_info = {};
  render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_info.attachmentCount = 1;
  render_pass_info.pAttachments = &attachment;
  render_pass_info.subpassCount = 1;
  render_pass_info.pSubpasses = &subpass;
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  VkRenderPass render_pass = VK_NULL_HANDLE;
  if (vulkan_device->functions().vkCreateRenderPass(vulkan_device->device(), &render_pass_info,
                                                    nullptr, &render_pass) != VK_SUCCESS) {
    REXGPU_ERROR("Scene effects: failed to create a render pass (format {}, {} samples)",
                 uint32_t(format), samples);
    render_pass = VK_NULL_HANDLE;
  }
  render_passes_.emplace(key, render_pass);
  return render_pass;
}

VkPipeline VulkanSceneEffects::GetDrawPipeline(Draw draw, VkFormat format, uint32_t samples) {
  auto key = std::make_tuple(draw, format, samples);
  auto it = draw_pipelines_.find(key);
  if (it != draw_pipelines_.end()) {
    return it->second;
  }
  DrawLayout draw_layout = GetDrawLayout(draw);
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = fullscreen_vs_;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = draw_ps_[size_t(draw_layout)];
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex_input = {};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport_state = {};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rasterization = {};
  rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization.cullMode = VK_CULL_MODE_NONE;
  rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterization.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample = {};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VkSampleCountFlagBits(samples);
  // Multiply: dest * source; add: dest + source; others replace. Alpha is
  // untouched.
  VkPipelineColorBlendAttachmentState blend = {};
  blend.blendEnable = (draw == Draw::kCompositeMultiply || draw == Draw::kCompositeAdd);
  blend.srcColorBlendFactor =
      draw == Draw::kCompositeAdd ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
  blend.dstColorBlendFactor =
      draw == Draw::kCompositeAdd ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_COLOR;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
  VkPipelineColorBlendStateCreateInfo color_blend = {};
  color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend.attachmentCount = 1;
  color_blend.pAttachments = &blend;
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic_state = {};
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.dynamicStateCount = uint32_t(std::size(dynamic_states));
  dynamic_state.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering_info = {};
  rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering_info.colorAttachmentCount = 1;
  rendering_info.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo pipeline_info = {};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.pNext = dynamic_rendering_ ? &rendering_info : nullptr;
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &rasterization;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pColorBlendState = &color_blend;
  pipeline_info.pDynamicState = &dynamic_state;
  pipeline_info.layout = draw_pipeline_layouts_[size_t(draw_layout)];
  pipeline_info.renderPass = dynamic_rendering_ ? VK_NULL_HANDLE : GetRenderPass(format, samples);
  const ui::vulkan::VulkanDevice* vulkan_device = command_processor_.GetVulkanDevice();
  VkPipeline pipeline = VK_NULL_HANDLE;
  if ((!dynamic_rendering_ && pipeline_info.renderPass == VK_NULL_HANDLE) ||
      vulkan_device->functions().vkCreateGraphicsPipelines(vulkan_device->device(),
                                                           VK_NULL_HANDLE, 1, &pipeline_info,
                                                           nullptr, &pipeline) != VK_SUCCESS) {
    REXGPU_ERROR("Scene effects: failed to create draw pipeline {} (format {}, {} samples)",
                 uint32_t(draw), uint32_t(format), samples);
    pipeline = VK_NULL_HANDLE;
  }
  draw_pipelines_.emplace(key, pipeline);
  return pipeline;
}

}  // namespace rex::graphics::vulkan
