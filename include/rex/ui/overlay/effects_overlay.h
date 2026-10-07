/**
 * @file        rex/ui/overlay/effects_overlay.h
 *
 * @brief       In-game menu for the GPU plugin's modern graphics effects.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

// Enables, disables and tunes each effect live (the "GPU/Effects" cvars of the
// GPU plugin), with quality presets, debug views and save-to-config. Effects
// the running GPU backend doesn't provide are hidden.
class EffectsDialog : public ImGuiDialog {
 public:
  // config_path: where "Save" writes (the same file as the settings overlay).
  EffectsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path);
  ~EffectsDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void ApplyPreset(int preset);
  void ResetToDefaults();

  std::filesystem::path config_path_;
  std::string status_;
};

}  // namespace rex::ui
