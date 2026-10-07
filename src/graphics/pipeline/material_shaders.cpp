/**
 * @file        graphics/pipeline/material_shaders.cpp
 * @brief       Hand-written replacements for the guest's shaders
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/pipeline/material_shaders.h>

#include <bit>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#define REX_MATERIAL_LIVE .lifecycle(rex::cvar::Lifecycle::kHotReload)

REXCVAR_DEFINE_BOOL(material_shaders, true, "GPU/Materials",
                    "Replace the game's own shaders with the improved material shaders "
                    "(restart to apply)");
REXCVAR_DEFINE_STRING(material_shaders_path, "materials", "GPU/Materials",
                      "Folder of the material shaders, relative to the executable");

REXCVAR_DEFINE_BOOL(material_soft_shadows, true, "GPU/Materials",
                    "Soft sun shadows: penumbras widen with the distance from what casts them")
    REX_MATERIAL_LIVE;
REXCVAR_DEFINE_DOUBLE(material_shadow_softness, 1.0, "GPU/Materials",
                      "Size of the sun for soft shadows - larger is softer")
    .range(0.25, 4.0) REX_MATERIAL_LIVE;
REXCVAR_DEFINE_BOOL(material_specular, true, "GPU/Materials",
                    "Physically based highlights (GGX with Fresnel) instead of the game's "
                    "Phong")
    REX_MATERIAL_LIVE;
REXCVAR_DEFINE_DOUBLE(material_specular_intensity, 1.0, "GPU/Materials",
                      "Strength of the physically based highlights")
    .range(0.0, 4.0) REX_MATERIAL_LIVE;
REXCVAR_DEFINE_DOUBLE(material_roughness, 0.45, "GPU/Materials",
                      "Surface roughness - lower gives smaller, sharper highlights")
    .range(0.05, 1.0) REX_MATERIAL_LIVE;

namespace rex::graphics::material_shaders {

namespace {

std::filesystem::path Utf8Path(std::string_view text) {
  return std::filesystem::u8path(text.begin(), text.end());
}

}  // namespace

bool IsEnabled() { return REXCVAR_GET(material_shaders); }

bool Load(uint64_t ucode_hash, uint64_t modification, std::string_view backend,
          std::string_view extension, std::vector<uint8_t>& binary_out) {
  std::filesystem::path directory = Utf8Path(REXCVAR_GET(material_shaders_path));
  if (directory.is_relative()) {
    directory = rex::filesystem::GetExecutableFolder() / directory;
  }
  directory /= Utf8Path(backend);
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    return false;
  }
  // The exact modification first, then one for any.
  std::filesystem::path paths[] = {
      directory /
          Utf8Path(fmt::format("{:016X}_{:016X}.{}", ucode_hash, modification, extension)),
      directory / Utf8Path(fmt::format("{:016X}.{}", ucode_hash, extension)),
  };
  for (const std::filesystem::path& path : paths) {
    if (!std::filesystem::is_regular_file(path, error)) {
      continue;
    }
    FILE* file = rex::filesystem::OpenFile(path, "rb");
    if (!file) {
      continue;
    }
    std::vector<uint8_t> binary(size_t(std::filesystem::file_size(path, error)));
    bool read = !error && !binary.empty() &&
                fread(binary.data(), 1, binary.size(), file) == binary.size();
    fclose(file);
    if (!read) {
      REXGPU_WARN("Material shader {} could not be read", path.filename().string());
      continue;
    }
    REXGPU_INFO("Material shader {:016X} (modification {:016X}): {}", ucode_hash, modification,
                path.filename().string());
    binary_out = std::move(binary);
    return true;
  }
  return false;
}

void GetParams(float params_out[kMaterialParamsCount][4], uint32_t draw_resolution_scale_x,
               uint32_t draw_resolution_scale_y, uint32_t translation_flags) {
  uint32_t features = 0;
  if (REXCVAR_GET(material_soft_shadows)) {
    features |= kMaterialFeatureSoftShadows;
  }
  if (REXCVAR_GET(material_specular)) {
    features |= kMaterialFeatureSpecular;
  }
  params_out[0][0] = std::bit_cast<float>(features);
  params_out[0][1] = float(REXCVAR_GET(material_shadow_softness));
  params_out[0][2] = float(REXCVAR_GET(material_specular_intensity));
  params_out[0][3] = float(REXCVAR_GET(material_roughness));
  for (uint32_t i = 1; i < 3; ++i) {
    for (uint32_t j = 0; j < 4; ++j) {
      params_out[i][j] = 0.0f;
    }
  }
  params_out[3][0] = float(draw_resolution_scale_x);
  params_out[3][1] = float(draw_resolution_scale_y);
  params_out[3][2] = std::bit_cast<float>(translation_flags);
  params_out[3][3] = 0.0f;
}

}  // namespace rex::graphics::material_shaders
