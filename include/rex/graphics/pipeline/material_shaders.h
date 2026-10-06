/**
 * @file        graphics/pipeline/material_shaders.h
 * @brief       Hand-written replacements for the guest's shaders
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace rex::graphics::material_shaders {

// Material shaders replace the translations of the game's own shaders with
// improved host shaders - better lighting, shadows and materials, rather than
// effects on top of the finished image. One is found by the guest shader's
// fingerprint (ucode hash) and the translation's modification bits:
//   <material_shaders_path>/<backend>/<HASH>_<MODIFICATION>.<extension>
//   <material_shaders_path>/<backend>/<HASH>.<extension> (any modification)
// It takes the translation's place with the same resource bindings (written
// by dump_shaders next to each translation, in .bindings.txt), and gets the
// material settings in the system constants (xe_material_params) - see
// MaterialParams.
//
// The parameters (kMaterialParamsCount float4s, after the translator's own
// system constants):
// 0.x - Features (uint bits, MaterialFeature).
// 0.y - Soft shadow light size scale.
// 0.z - Specular intensity.
// 0.w - Surface roughness.
// 1, 2 - Reserved.
// 3.x, 3.y - Draw resolution scale.
// 3.z - Translation flags (uint bits, TranslationFlag) - what the translator
//       bakes into its shaders.
// 3.w - Reserved.
constexpr uint32_t kMaterialParamsCount = 4;

enum MaterialFeature : uint32_t {
  // Penumbras that widen with the distance from the caster.
  kMaterialFeatureSoftShadows = 1u << 0,
  // GGX specular with Fresnel instead of the game's Phong.
  kMaterialFeatureSpecular = 1u << 1,
};

enum TranslationFlag : uint32_t {
  kTranslationFlagGammaRenderTargetAsUnorm8 = 1u << 0,
  kTranslationFlagMsaa2xSupported = 1u << 1,
  kTranslationFlagFuzzyAlphaEpsilon = 1u << 2,
  kTranslationFlagScaledTextureOffsets = 1u << 3,
};

// Whether material shaders replace translations (read when translating).
bool IsEnabled();

// The material shader for a translation, if there is one. backend: the
// subdirectory ("d3d12"), extension: the file extension ("dxbc").
bool Load(uint64_t ucode_hash, uint64_t modification, std::string_view backend,
          std::string_view extension, std::vector<uint8_t>& binary_out);

// The system constants' material parameters for the current settings.
void GetParams(float params_out[kMaterialParamsCount][4], uint32_t draw_resolution_scale_x,
               uint32_t draw_resolution_scale_y, uint32_t translation_flags);

}  // namespace rex::graphics::material_shaders
