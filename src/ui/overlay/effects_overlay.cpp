/**
 * @file        ui/overlay/effects_overlay.cpp
 *
 * @brief       In-game graphics effects menu. See effects_overlay.h for details.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/effects_overlay.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace rex::ui {

namespace {

constexpr std::string_view kEffectsCategory = "GPU/Effects";
constexpr std::string_view kMaterialsCategory = "GPU/Materials";

const rex::cvar::FlagEntry* FindCvar(std::string_view name) {
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

bool HasCvar(std::string_view name) { return FindCvar(name) != nullptr; }

bool GetBool(std::string_view name) { return rex::cvar::GetFlagByName(name) == "true"; }

void SetBool(std::string_view name, bool value) {
  rex::cvar::SetFlagByName(name, value ? "true" : "false");
}

double GetDouble(std::string_view name) {
  return std::strtod(rex::cvar::GetFlagByName(name).c_str(), nullptr);
}

void SetDouble(std::string_view name, double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.6g", value);
  rex::cvar::SetFlagByName(name, text);
}

// Each widget writes the cvar only when the user changes it.
void CheckboxCvar(const char* label, std::string_view name) {
  bool value = GetBool(name);
  if (ImGui::Checkbox(label, &value)) {
    SetBool(name, value);
  }
}

void SliderCvar(const char* label, std::string_view name, float min, float max,
                const char* format = "%.2f", ImGuiSliderFlags flags = 0) {
  float value = float(GetDouble(name));
  if (ImGui::SliderFloat(label, &value, min, max, format, flags)) {
    SetDouble(name, value);
  }
}

// An integer cvar: item i is the value values[i].
void ComboCvar(const char* label, std::string_view name, const char* const* items,
               const int* values, int count) {
  int current = std::atoi(rex::cvar::GetFlagByName(name).c_str());
  int index = 0;
  for (int i = 0; i < count; ++i) {
    if (values[i] == current) {
      index = i;
    }
  }
  if (ImGui::Combo(label, &index, items, count)) {
    rex::cvar::SetFlagByName(name, std::to_string(values[index]));
  }
}

// A string cvar with a fixed set of values, shown with friendlier labels.
void StringComboCvar(const char* label, std::string_view name, const char* const* values,
                     const char* const* labels, int count) {
  std::string current = rex::cvar::GetFlagByName(name);
  int index = 0;
  for (int i = 0; i < count; ++i) {
    if (current == values[i]) {
      index = i;
    }
  }
  if (ImGui::Combo(label, &index, labels, count)) {
    rex::cvar::SetFlagByName(name, values[index]);
  }
}

// "r,g,b" string cvars.
void ColorCvar(const char* label, std::string_view name) {
  float color[3] = {1.0f, 1.0f, 1.0f};
  std::string text = rex::cvar::GetFlagByName(name);
  const char* cursor = text.c_str();
  for (float& component : color) {
    char* end;
    float parsed = std::strtof(cursor, &end);
    if (end == cursor) {
      break;
    }
    component = parsed;
    cursor = end;
    while (*cursor == ',' || *cursor == ' ') {
      ++cursor;
    }
  }
  if (ImGui::ColorEdit3(label, color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) {
    char value[64];
    std::snprintf(value, sizeof(value), "%.3f,%.3f,%.3f", color[0], color[1], color[2]);
    rex::cvar::SetFlagByName(name, value);
  }
}

void Description(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

// A collapsing section headed by its effect's enable checkbox.
bool EffectHeader(const char* title, std::string_view enable_cvar) {
  ImGui::PushID(title);
  bool enabled = GetBool(enable_cvar);
  if (ImGui::Checkbox("##enabled", &enabled)) {
    SetBool(enable_cvar, enabled);
  }
  ImGui::SameLine();
  bool open = ImGui::CollapsingHeader(title);
  ImGui::PopID();
  return open;
}

const char* const kQualities[] = {"Low", "Medium", "High", "Ultra"};
const int kQualityValues[] = {0, 1, 2, 3};
const char* const kPresets[] = {"Off", "Low", "Medium", "High", "Ultra"};
const char* const kDebugViews[] = {"Off",
                                   "Split screen (effects on the left)",
                                   "Ambient occlusion",
                                   "Volumetric lighting",
                                   "Global illumination",
                                   "Fog",
                                   "Contact shadows",
                                   "Reflections"};
const int kDebugValues[] = {0, 1, 2, 3, 4, 5, 6, 7};
const char* const kResolutionScales[] = {"1x (native)", "2x", "3x"};
const int kResolutionScaleValues[] = {1, 2, 3};
const char* const kAnisotropy[] = {"Game's own", "Off", "2x", "4x", "8x", "16x"};
const int kAnisotropyValues[] = {-1, 0, 2, 3, 4, 5};
const char* const kAntiAliasingValues[] = {"none", "fxaa", "fxaa_extreme"};
const char* const kAntiAliasingLabels[] = {"Off", "FXAA", "FXAA (extreme)"};

}  // namespace

EffectsDialog::EffectsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path)
    : ImGuiDialog(imgui_drawer), config_path_(std::move(config_path)) {}

EffectsDialog::~EffectsDialog() {}

void EffectsDialog::ApplyPreset(int preset) {
  // Which effects run, and their quality - the look (strengths, colors, fog)
  // is kept.
  struct Preset {
    bool ao, contact_shadows, fog, volumetrics, gi, full_resolution;
    int ao_quality, volumetrics_quality;
  };
  static const Preset kPresetValues[] = {
      {false, false, false, false, false, false, 0, 0},  // Off
      {true, false, true, false, false, false, 0, 0},    // Low
      {true, true, true, true, false, false, 1, 0},      // Medium
      {true, true, true, true, false, false, 2, 2},      // High
      {true, true, true, true, true, true, 3, 3},        // Ultra
  };
  const Preset& values = kPresetValues[preset];
  SetBool("scene_fx_ao", values.ao);
  SetBool("scene_fx_contact_shadows", values.contact_shadows);
  SetBool("scene_fx_fog", values.fog);
  SetBool("scene_fx_volumetrics", values.volumetrics);
  SetBool("scene_fx_gi", values.gi);
  SetBool("scene_fx_ao_full_resolution", values.full_resolution);
  rex::cvar::SetFlagByName("scene_fx_ao_quality", std::to_string(values.ao_quality));
  rex::cvar::SetFlagByName("scene_fx_volumetrics_quality",
                           std::to_string(values.volumetrics_quality));
  if (preset == 0) {
    SetBool("scene_fx_reflections", false);
  }
  status_ = std::string("Preset: ") + kPresets[preset];
}

void EffectsDialog::ResetToDefaults() {
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (entry.category == kEffectsCategory || entry.category == kMaterialsCategory) {
      rex::cvar::SetFlagByName(entry.name, entry.default_value);
    }
  }
  status_ = "Defaults restored";
}

void EffectsDialog::OnDraw(ImGuiIO& /*io*/) {
  ImGui::SetNextWindowSize(ImVec2(500, 640), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.92f);
  if (!ImGui::Begin("Graphics Enhancements (F6)##rex_effects", nullptr,
                    ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  if (!HasCvar("scene_fx_ao")) {
    ImGui::TextWrapped("The running GPU backend doesn't provide the graphics enhancements.");
    ImGui::End();
    return;
  }
  ImGui::PushItemWidth(-180.0f);

  ImGui::TextUnformatted("Preset");
  for (int i = 0; i < int(std::size(kPresets)); ++i) {
    ImGui::SameLine();
    if (ImGui::Button(kPresets[i])) {
      ApplyPreset(i);
    }
  }
  Description("Tick an effect to turn it on; open it to tune it. Changes apply instantly.");
  ImGui::Separator();

  if (EffectHeader("Ambient Occlusion", "scene_fx_ao")) {
    ImGui::PushID("ao");
    Description(
        "Ground-truth ambient occlusion with visibility bitmasks: soft shadowing in corners, "
        "creases and under objects, without dark halos around thin objects.");
    ComboCvar("Quality", "scene_fx_ao_quality", kQualities, kQualityValues, 4);
    SliderCvar("Strength", "scene_fx_ao_strength", 0.0f, 1.0f);
    SliderCvar("Radius", "scene_fx_ao_radius", 0.25f, 5.0f, "%.2f m");
    SliderCvar("Contrast", "scene_fx_ao_power", 0.5f, 3.0f);
    SliderCvar("Object thickness", "scene_fx_ao_thickness", 0.05f, 2.0f, "%.2f m");
    SliderCvar("Light bounce", "scene_fx_ao_albedo", 0.0f, 0.9f);
    CheckboxCvar("Full resolution (also GI, contact shadows, reflections)",
                 "scene_fx_ao_full_resolution");
    ImGui::PopID();
  }

  if (EffectHeader("Global Illumination", "scene_fx_gi")) {
    ImGui::PushID("gi");
    Description(
        "Light bouncing between nearby surfaces - color bleeding and softly lit shadows - "
        "gathered with the ambient occlusion's rays (its quality, radius and resolution "
        "apply).");
    SliderCvar("Strength", "scene_fx_gi_intensity", 0.0f, 4.0f);
    ImGui::PopID();
  }

  if (EffectHeader("Contact Shadows", "scene_fx_contact_shadows")) {
    ImGui::PushID("contact");
    Description(
        "Fine sun shadows the game's shadow maps are too coarse for: under feet and props, in "
        "grass and crevices.");
    SliderCvar("Strength", "scene_fx_contact_shadows_strength", 0.0f, 1.0f);
    SliderCvar("Length", "scene_fx_contact_shadows_length", 0.05f, 3.0f, "%.2f m");
    SliderCvar("Object thickness", "scene_fx_contact_shadows_thickness", 0.02f, 2.0f,
               "%.2f m");
    ImGui::PopID();
  }

  if (EffectHeader("Volumetric Lighting", "scene_fx_volumetrics")) {
    ImGui::PushID("volumetrics");
    Description(
        "Light shafts: sunlight scattering in the air, shadowed by the game's own sun shadows "
        "- through buildings, trees and windows.");
    ComboCvar("Quality", "scene_fx_volumetrics_quality", kQualities, kQualityValues, 4);
    SliderCvar("Brightness", "scene_fx_volumetrics_intensity", 0.0f, 4.0f);
    SliderCvar("Haze", "scene_fx_volumetrics_density", 0.0f, 0.03f, "%.4f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Glow toward the sun", "scene_fx_volumetrics_anisotropy", 0.0f, 0.95f);
    SliderCvar("Distance", "scene_fx_volumetrics_distance", 20.0f, 500.0f, "%.0f m");
    ColorCvar("Sunlight color", "scene_fx_sun_tint");
    CheckboxCvar("Smooth over frames (temporal)", "scene_fx_volumetrics_temporal");
    CheckboxCvar("Full resolution", "scene_fx_volumetrics_full_resolution");
    ImGui::PopID();
  }

  if (EffectHeader("Fog", "scene_fx_fog")) {
    ImGui::PushID("fog");
    Description(
        "Height fog lit by the sky, with a glow toward the sun - thicker near the ground and "
        "in valleys.");
    SliderCvar("Density", "scene_fx_fog_density", 0.0f, 0.02f, "%.4f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Ground fog", "scene_fx_fog_ground_density", 0.0f, 0.2f, "%.4f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Ground fog height", "scene_fx_fog_ground_height", -20.0f, 20.0f, "%.1f m");
    SliderCvar("Ground fog falloff", "scene_fx_fog_ground_falloff", 0.02f, 2.0f, "%.3f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Brightness", "scene_fx_fog_brightness", 0.0f, 2.0f);
    SliderCvar("Sun glow", "scene_fx_fog_sun_glow", 0.0f, 2.0f);
    ImGui::PopID();
  }

  if (EffectHeader("Reflections", "scene_fx_reflections")) {
    ImGui::PushID("reflections");
    Description(
        "Screen-space reflections. The game doesn't say which surfaces are glossy, so all "
        "reflect a little, most at grazing angles - or only floors and streets.");
    ComboCvar("Quality", "scene_fx_reflections_quality", kQualities, kQualityValues, 4);
    SliderCvar("Strength", "scene_fx_reflections_intensity", 0.0f, 4.0f);
    SliderCvar("Shininess", "scene_fx_reflections_reflectance", 0.0f, 0.5f, "%.3f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Distance", "scene_fx_reflections_distance", 2.0f, 100.0f, "%.0f m");
    CheckboxCvar("Floors only", "scene_fx_reflections_floors_only");
    ImGui::PopID();
  }

  if (EffectHeader("Sharpening", "scene_fx_sharpen")) {
    ImGui::PushID("sharpen");
    Description(
        "Contrast-adaptive sharpening of the final image: crisper detail without halos (no "
        "upscaling).");
    SliderCvar("Strength", "scene_fx_sharpen_amount", 0.0f, 1.0f);
    ImGui::PopID();
  }

  if (EffectHeader("Color Grading", "scene_fx_grading")) {
    ImGui::PushID("grading");
    SliderCvar("Exposure", "scene_fx_grading_exposure", 0.25f, 4.0f, "%.2f",
               ImGuiSliderFlags_Logarithmic);
    SliderCvar("Contrast", "scene_fx_grading_contrast", 0.5f, 2.0f);
    SliderCvar("Saturation", "scene_fx_grading_saturation", 0.0f, 2.0f);
    SliderCvar("Vibrance", "scene_fx_grading_vibrance", -1.0f, 1.0f);
    SliderCvar("Temperature", "scene_fx_grading_temperature", -1.0f, 1.0f);
    SliderCvar("Tint", "scene_fx_grading_tint", -1.0f, 1.0f);
    SliderCvar("Gamma", "scene_fx_grading_gamma", 0.5f, 2.0f);
    SliderCvar("Shadow lift", "scene_fx_grading_shadow_lift", 0.0f, 0.5f);
    SliderCvar("Vignette", "scene_fx_vignette", 0.0f, 1.0f);
    SliderCvar("Film grain", "scene_fx_film_grain", 0.0f, 1.0f);
    ImGui::PopID();
  }

  if (HasCvar("material_shaders") && ImGui::CollapsingHeader("Materials")) {
    ImGui::PushID("materials");
    Description(
        "Rewritten versions of the game's own shaders with modern lighting, replacing its "
        "rendering rather than adding to the finished image (Direct3D 12).");
    CheckboxCvar("Material shaders (applies after restarting)", "material_shaders");
    CheckboxCvar("Soft sun shadows", "material_soft_shadows");
    Description(
        "Percentage-closer soft shadows: sharp where objects touch the ground, softer the "
        "further the shadow falls.");
    SliderCvar("Sun size", "material_shadow_softness", 0.25f, 4.0f);
    CheckboxCvar("Physically based highlights", "material_specular");
    Description(
        "GGX highlights with Fresnel from each surface's specular map, and reflections "
        "strongest at grazing angles.");
    SliderCvar("Highlight strength", "material_specular_intensity", 0.0f, 4.0f);
    SliderCvar("Roughness", "material_roughness", 0.05f, 1.0f);
    ImGui::PopID();
  }

  if (ImGui::CollapsingHeader("Rendering")) {
    ImGui::PushID("rendering");
    if (HasCvar("anisotropic_override")) {
      ComboCvar("Texture filtering", "anisotropic_override", kAnisotropy, kAnisotropyValues,
                int(std::size(kAnisotropy)));
      Description("Anisotropic filtering keeps textures sharp at oblique angles.");
    }
    if (HasCvar("resolution_scale")) {
      ComboCvar("Internal resolution", "resolution_scale", kResolutionScales,
                kResolutionScaleValues, int(std::size(kResolutionScales)));
      Description(
          "Renders the game at a multiple of its resolution - much sharper, but the GPU cost "
          "grows with the square. Applies after restarting the game.");
    }
    if (HasCvar("swap_post_effect")) {
      StringComboCvar("Anti-aliasing", "swap_post_effect", kAntiAliasingValues,
                      kAntiAliasingLabels, int(std::size(kAntiAliasingValues)));
      Description("Applies after restarting the game.");
    }
    ImGui::PopID();
  }

  if (ImGui::CollapsingHeader("Debug")) {
    ImGui::PushID("debug");
    ComboCvar("View", "scene_fx_debug", kDebugViews, kDebugValues, int(std::size(kDebugViews)));
    ImGui::PopID();
  }

  ImGui::PopItemWidth();
  ImGui::Separator();
  if (ImGui::Button("Save")) {
    rex::cvar::SaveConfig(config_path_);
    status_ = "Saved to " + config_path_.filename().string();
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset to defaults")) {
    ResetToDefaults();
  }
  if (!status_.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", status_.c_str());
  }
  ImGui::End();
}

}  // namespace rex::ui
