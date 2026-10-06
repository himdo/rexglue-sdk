/**
 * @file        graphics/d3d12/scene_effects.cpp
 * @brief       Direct3D 12 backend of the modern graphics effects
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/d3d12/scene_effects.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include <rex/assert.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/render_target_cache.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_util.h>

namespace rex::graphics::d3d12 {

namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/scene_fx_ao_blur_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_ao_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_ao_gi_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_ao_prefilter_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_blur_rgba_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_color_capture_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_color_capture_msaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_color_copy_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_color_copy_msaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_composite_ps.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_contact_shadows_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_depth_copy_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_depth_copy_msaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_fullscreen_vs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_image_ps.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_reflections_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_shadow_copy_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_sky_color_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_sky_color_msaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_temporal_cs.h"
#include "../shaders/bytecode/d3d12_5_1/scene_fx_volumetric_cs.h"
}  // namespace shaders

namespace {

constexpr uint32_t kRootConstantCount = 32;
constexpr UINT kComputeSourceParameter = 1;
constexpr UINT kComputeTargetParameter = kComputeSourceParameter + 8;
constexpr UINT kComputeExtraConstantsParameter = kComputeTargetParameter + 4;
constexpr UINT kDrawSourceParameter = 1;

struct ShaderCode {
  const void* code;
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

// Read by both compute and pixel shaders without transitions in between.
constexpr D3D12_RESOURCE_STATES kShaderReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

}  // namespace

D3D12SceneEffects::D3D12SceneEffects(D3D12CommandProcessor& command_processor,
                                     D3D12RenderTargetCache& render_target_cache,
                                     const RegisterFile& register_file)
    : SceneEffects(render_target_cache, register_file),
      command_processor_(command_processor),
      d3d12_render_target_cache_(render_target_cache) {
  static_assert(std::size(kComputeShaders) == size_t(Pipeline::kCount));
}

D3D12SceneEffects::~D3D12SceneEffects() { Shutdown(); }

bool D3D12SceneEffects::Initialize() {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxAnisotropy = 1;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  // Compute: constants, single-descriptor tables, a root CBV.
  D3D12_DESCRIPTOR_RANGE compute_ranges[kMaxSources + kMaxTargets] = {};
  D3D12_ROOT_PARAMETER compute_parameters[kComputeExtraConstantsParameter + 1] = {};
  compute_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  compute_parameters[0].Constants.Num32BitValues = kRootConstantCount;
  compute_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  static_assert(kMaxSources == 8 && kMaxTargets == 4);
  for (uint32_t i = 0; i < kMaxSources + kMaxTargets; ++i) {
    D3D12_DESCRIPTOR_RANGE& range = compute_ranges[i];
    bool is_source = i < kMaxSources;
    range.RangeType =
        is_source ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = is_source ? i : i - kMaxSources;
    D3D12_ROOT_PARAMETER& parameter = compute_parameters[kComputeSourceParameter + i];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_PARAMETER& compute_extra = compute_parameters[kComputeExtraConstantsParameter];
  compute_extra.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  compute_extra.Descriptor.ShaderRegister = 1;
  compute_extra.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
  root_signature_desc.NumParameters = UINT(std::size(compute_parameters));
  root_signature_desc.pParameters = compute_parameters;
  root_signature_desc.NumStaticSamplers = 1;
  root_signature_desc.pStaticSamplers = &sampler;
  compute_root_signature_ = ui::d3d12::util::CreateRootSignature(provider, root_signature_desc);

  // Draws: constants, single-descriptor tables, a root CBV.
  D3D12_DESCRIPTOR_RANGE draw_ranges[kDrawSourceCount] = {};
  D3D12_ROOT_PARAMETER draw_parameters[kDrawSourceParameter + kDrawSourceCount + 1] = {};
  draw_parameters[0] = compute_parameters[0];
  for (uint32_t i = 0; i < kDrawSourceCount; ++i) {
    draw_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    draw_ranges[i].NumDescriptors = 1;
    draw_ranges[i].BaseShaderRegister = i;
    D3D12_ROOT_PARAMETER& parameter = draw_parameters[kDrawSourceParameter + i];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &draw_ranges[i];
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  D3D12_ROOT_PARAMETER& draw_extra = draw_parameters[kDrawSourceParameter + kDrawSourceCount];
  draw_extra.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  draw_extra.Descriptor.ShaderRegister = 1;
  draw_extra.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  root_signature_desc.NumParameters = UINT(std::size(draw_parameters));
  root_signature_desc.pParameters = draw_parameters;
  draw_root_signature_ = ui::d3d12::util::CreateRootSignature(provider, root_signature_desc);
  if (!compute_root_signature_ || !draw_root_signature_) {
    REXGPU_ERROR("Scene effects: failed to create the root signatures");
    return false;
  }

  for (uint32_t i = 0; i < uint32_t(Pipeline::kCount); ++i) {
    pipelines_[i] = ui::d3d12::util::CreateComputePipeline(
        device, kComputeShaders[i].code, kComputeShaders[i].size, compute_root_signature_);
    if (!pipelines_[i]) {
      REXGPU_ERROR("Scene effects: failed to create compute pipeline {}", i);
      return false;
    }
  }
  upload_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(provider, 64 * 1024);
  return true;
}

void D3D12SceneEffects::Shutdown() {
  for (auto& pipeline : draw_pipelines_) {
    if (pipeline.second) {
      pipeline.second->Release();
    }
  }
  draw_pipelines_.clear();
  for (ID3D12PipelineState*& pipeline : pipelines_) {
    ui::d3d12::util::ReleaseAndNull(pipeline);
  }
  ui::d3d12::util::ReleaseAndNull(draw_root_signature_);
  ui::d3d12::util::ReleaseAndNull(compute_root_signature_);
  upload_pool_.reset();
  // The command processor has awaited the GPU before shutting down.
  for (TextureResource& texture : textures_) {
    ReleaseTexture(texture, true);
  }
  for (auto& resource : resources_to_release_) {
    resource.second->Release();
  }
  resources_to_release_.clear();
}

void D3D12SceneEffects::ReleaseCompletedResources() {
  uint64_t completed = command_processor_.GetCompletedSubmission();
  std::erase_if(resources_to_release_, [completed](const auto& entry) {
    if (entry.first > completed) {
      return false;
    }
    entry.second->Release();
    return true;
  });
  if (upload_pool_) {
    upload_pool_->Reclaim(completed);
  }
}

uint64_t D3D12SceneEffects::GetCurrentFrame() const {
  return command_processor_.GetCurrentFrame();
}

DXGI_FORMAT D3D12SceneEffects::GetFormat(TextureFormat format) {
  switch (format) {
    case TextureFormat::kR32Float:
      return DXGI_FORMAT_R32_FLOAT;
    case TextureFormat::kR16Float:
      return DXGI_FORMAT_R16_FLOAT;
    case TextureFormat::kRGBA16Float:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
  }
  return DXGI_FORMAT_UNKNOWN;
}

bool D3D12SceneEffects::EnsureTexture(Texture texture, uint32_t width, uint32_t height) {
  TextureResource& resource = textures_[size_t(texture)];
  if (resource.resource && resource.width >= width && resource.height >= height) {
    return true;
  }
  ReleaseTexture(resource, false);
  const TextureInfo& info = kTextureInfos[size_t(texture)];
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = std::max(width, resource.width);
  desc.Height = std::max(height, resource.height);
  desc.DepthOrArraySize = 1;
  desc.MipLevels = UINT16(info.mip_levels);
  desc.Format = GetFormat(info.format);
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(provider.GetDevice()->CreateCommittedResource(
          &ui::d3d12::util::kHeapPropertiesDefault,
          info.zeroed ? D3D12_HEAP_FLAG_NONE : provider.GetHeapFlagCreateNotZeroed(), &desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&resource.resource)))) {
    resource.resource = nullptr;
    return false;
  }
  resource.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  resource.width = uint32_t(desc.Width);
  resource.height = desc.Height;
  return true;
}

void D3D12SceneEffects::ReleaseTexture(TextureResource& texture, bool immediately) {
  if (!texture.resource) {
    return;
  }
  if (immediately) {
    texture.resource->Release();
  } else {
    resources_to_release_.emplace_back(command_processor_.GetCurrentSubmission(),
                                       texture.resource);
  }
  texture.resource = nullptr;
}

void D3D12SceneEffects::TransitionTexture(Texture texture, D3D12_RESOURCE_STATES new_state) {
  TextureResource& resource = textures_[size_t(texture)];
  command_processor_.PushTransitionBarrier(resource.resource, resource.state, new_state);
  resource.state = new_state;
}

void D3D12SceneEffects::TransitionRenderTarget(RenderTarget& render_target,
                                               D3D12_RESOURCE_STATES new_state) {
  auto& d3d12_rt = static_cast<D3D12RenderTargetCache::D3D12RenderTarget&>(render_target);
  command_processor_.PushTransitionBarrier(d3d12_rt.resource(), d3d12_rt.SetResourceState(new_state),
                                           new_state);
}

uint32_t D3D12SceneEffects::GetHostSampleCount(const RenderTarget& render_target) const {
  xenos::MsaaSamples samples = render_target.key().msaa_samples;
  // 2x is emulated with 4x where the host lacks it.
  if (samples == xenos::MsaaSamples::k2X && !d3d12_render_target_cache_.msaa_2x_supported()) {
    return 4;
  }
  return 1u << uint32_t(samples);
}

void D3D12SceneEffects::WriteSourceView(D3D12_CPU_DESCRIPTOR_HANDLE handle, const Source& source,
                                        D3D12_RESOURCE_STATES read_state) {
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  if (source.render_target) {
    auto& d3d12_rt = static_cast<D3D12RenderTargetCache::D3D12RenderTarget&>(*source.render_target);
    TransitionRenderTarget(*source.render_target, read_state);
    if (d3d12_rt.key().is_depth) {
      // The cache's own view of the depth.
      device->CopyDescriptorsSimple(1, handle, d3d12_rt.descriptor_srv().GetHandle(),
                                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      return;
    }
    // The cache's color view is for bit-exact transfers - read it as floats.
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Format = d3d12_render_target_cache_.GetColorDrawDXGIFormat(d3d12_rt.key().GetColorFormat());
    if (GetHostSampleCount(d3d12_rt) > 1) {
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    } else {
      desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      desc.Texture2D.MipLevels = 1;
    }
    device->CreateShaderResourceView(d3d12_rt.resource(), &desc, handle);
    return;
  }
  TransitionTexture(source.texture, read_state);
  const TextureInfo& info = kTextureInfos[size_t(source.texture)];
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Format = GetFormat(info.format);
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  desc.Texture2D.MipLevels = info.mip_levels;
  device->CreateShaderResourceView(textures_[size_t(source.texture)].resource, &desc, handle);
}

D3D12_GPU_VIRTUAL_ADDRESS D3D12SceneEffects::UploadConstants(const void* data, size_t size) {
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
  uint8_t* mapping =
      upload_pool_->Request(command_processor_.GetCurrentSubmission(), (size + 255) & ~size_t(255),
                            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
                            &address);
  if (!mapping) {
    return 0;
  }
  std::memcpy(mapping, data, size);
  return address;
}

bool D3D12SceneEffects::Dispatch(Pipeline pipeline, std::initializer_list<Source> sources,
                                 std::initializer_list<Target> targets, const void* constants,
                                 const void* extra_constants, size_t extra_constants_size,
                                 uint32_t width, uint32_t height) {
  uint32_t source_count = uint32_t(sources.size());
  uint32_t target_count = uint32_t(targets.size());
  assert_true(source_count && source_count <= kMaxSources);
  assert_true(target_count && target_count <= kMaxTargets);
  D3D12_GPU_VIRTUAL_ADDRESS extra_address = 0;
  if (extra_constants_size) {
    extra_address = UploadConstants(extra_constants, extra_constants_size);
    if (!extra_address) {
      return false;
    }
  }
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[kMaxSources + kMaxTargets];
  if (!command_processor_.RequestOneUseSingleViewDescriptors(source_count + target_count,
                                                             descriptors)) {
    return false;
  }
  ID3D12Device* device = command_processor_.GetD3D12Provider().GetDevice();
  uint32_t descriptor_index = 0;
  for (const Source& source : sources) {
    // Guest render targets are only read by compute here.
    WriteSourceView(descriptors[descriptor_index++].first, source,
                    source.render_target ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                         : kShaderReadState);
  }
  for (const Target& target : targets) {
    TransitionTexture(target.texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format = GetFormat(kTextureInfos[size_t(target.texture)].format);
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = target.mip;
    device->CreateUnorderedAccessView(textures_[size_t(target.texture)].resource, nullptr, &desc,
                                      descriptors[descriptor_index++].first);
  }
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  command_list.D3DSetComputeRootSignature(compute_root_signature_);
  command_list.D3DSetComputeRoot32BitConstants(0, kRootConstantCount, constants, 0);
  // Unused tables get any valid descriptor of their kind.
  for (uint32_t i = 0; i < kMaxSources; ++i) {
    command_list.D3DSetComputeRootDescriptorTable(
        kComputeSourceParameter + i, descriptors[std::min(i, source_count - 1)].second);
  }
  for (uint32_t i = 0; i < kMaxTargets; ++i) {
    command_list.D3DSetComputeRootDescriptorTable(
        kComputeTargetParameter + i,
        descriptors[source_count + std::min(i, target_count - 1)].second);
  }
  if (extra_address) {
    command_list.D3DSetComputeRootConstantBufferView(kComputeExtraConstantsParameter,
                                                     extra_address);
  }
  command_processor_.SetExternalPipeline(pipelines_[size_t(pipeline)]);
  command_processor_.SubmitBarriers();
  command_list.D3DDispatch((width + 7) / 8, (height + 7) / 8, 1);
  return true;
}

bool D3D12SceneEffects::DrawFullscreen(Draw draw, RenderTarget& render_target, const Rect& rect,
                                       std::initializer_list<Source> sources,
                                       const void* constants, const void* extra_constants,
                                       size_t extra_constants_size) {
  auto& d3d12_rt = static_cast<D3D12RenderTargetCache::D3D12RenderTarget&>(render_target);
  DXGI_FORMAT format =
      d3d12_render_target_cache_.GetColorDrawDXGIFormat(d3d12_rt.key().GetColorFormat());
  ID3D12PipelineState* pipeline = GetDrawPipeline(draw, format, GetHostSampleCount(d3d12_rt));
  uint32_t source_count = uint32_t(sources.size());
  assert_true(source_count && source_count <= kDrawSourceCount);
  ui::d3d12::util::DescriptorCpuGpuHandlePair descriptors[kDrawSourceCount];
  if (!pipeline ||
      !command_processor_.RequestOneUseSingleViewDescriptors(source_count, descriptors)) {
    return false;
  }
  D3D12_GPU_VIRTUAL_ADDRESS extra_address = 0;
  if (extra_constants_size) {
    extra_address = UploadConstants(extra_constants, extra_constants_size);
    if (!extra_address) {
      return false;
    }
  }
  uint32_t descriptor_index = 0;
  for (const Source& source : sources) {
    WriteSourceView(descriptors[descriptor_index++].first, source, kShaderReadState);
  }
  d3d12_render_target_cache_.InvalidateCommandListRenderTargets();
  TransitionRenderTarget(render_target, D3D12_RESOURCE_STATE_RENDER_TARGET);
  command_processor_.SubmitBarriers();

  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3d12_rt.descriptor_draw().GetHandle();
  command_list.D3DOMSetRenderTargets(1, &rtv, FALSE, nullptr);
  command_processor_.SetExternalGraphicsRootSignature(draw_root_signature_);
  command_list.D3DSetGraphicsRoot32BitConstants(0, kRootConstantCount, constants, 0);
  for (uint32_t i = 0; i < kDrawSourceCount; ++i) {
    command_list.D3DSetGraphicsRootDescriptorTable(
        kDrawSourceParameter + i, descriptors[std::min(i, source_count - 1)].second);
  }
  if (extra_address) {
    command_list.D3DSetGraphicsRootConstantBufferView(kDrawSourceParameter + kDrawSourceCount,
                                                      extra_address);
  }
  command_processor_.SetExternalPipeline(pipeline);
  command_processor_.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  D3D12_VIEWPORT viewport = {float(rect.left),
                             float(rect.top),
                             float(rect.right - rect.left),
                             float(rect.bottom - rect.top),
                             0.0f,
                             1.0f};
  command_processor_.SetViewport(viewport);
  D3D12_RECT scissor = {rect.left, rect.top, rect.right, rect.bottom};
  command_processor_.SetScissorRect(scissor);
  command_list.D3DDrawInstanced(3, 1, 0, 0);
  return true;
}

ID3D12PipelineState* D3D12SceneEffects::GetDrawPipeline(Draw draw, DXGI_FORMAT format,
                                                        uint32_t samples) {
  auto key = std::make_tuple(draw, format, samples);
  auto it = draw_pipelines_.find(key);
  if (it != draw_pipelines_.end()) {
    return it->second;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  desc.pRootSignature = draw_root_signature_;
  desc.VS.pShaderBytecode = shaders::scene_fx_fullscreen_vs;
  desc.VS.BytecodeLength = sizeof(shaders::scene_fx_fullscreen_vs);
  if (draw == Draw::kImage) {
    desc.PS.pShaderBytecode = shaders::scene_fx_image_ps;
    desc.PS.BytecodeLength = sizeof(shaders::scene_fx_image_ps);
  } else {
    desc.PS.pShaderBytecode = shaders::scene_fx_composite_ps;
    desc.PS.BytecodeLength = sizeof(shaders::scene_fx_composite_ps);
  }
  // Multiply: dest * source; add: dest + source; others replace. Alpha is
  // untouched.
  D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
  blend.BlendEnable = draw == Draw::kCompositeMultiply || draw == Draw::kCompositeAdd;
  blend.SrcBlend = draw == Draw::kCompositeAdd ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
  blend.DestBlend = draw == Draw::kCompositeAdd ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_COLOR;
  blend.BlendOp = D3D12_BLEND_OP_ADD;
  blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
  blend.DestBlendAlpha = D3D12_BLEND_ONE;
  blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.LogicOp = D3D12_LOGIC_OP_NOOP;
  blend.RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = format;
  desc.SampleDesc.Count = samples;
  ID3D12PipelineState* pipeline = nullptr;
  if (FAILED(command_processor_.GetD3D12Provider().GetDevice()->CreateGraphicsPipelineState(
          &desc, IID_PPV_ARGS(&pipeline)))) {
    REXGPU_ERROR("Scene effects: failed to create draw pipeline {} (format {}, {} samples)",
                 uint32_t(draw), uint32_t(format), samples);
    pipeline = nullptr;
  }
  draw_pipelines_.emplace(key, pipeline);
  return pipeline;
}

}  // namespace rex::graphics::d3d12
