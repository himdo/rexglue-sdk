// Swap colour grade: a colour pass the GPU backend runs on the final guest
// output (after the gamma ramp, before FXAA, presentation and screenshot
// capture). The game sets it (photo mode filters); the GPU plugin reads it
// once per swap. Plain C entry points so both the game executable and the
// GPU plugin reach the one copy inside rexruntime.dll.
#pragma once

#include <cstdint>

namespace rex::ui {

struct SwapColorGrade {
  uint32_t enabled = 0;
  float contrast = 1.0f;  // power curve around the pivot (shadows[3])
  float gamma = 1.0f;     // > 1 darkens mid tones
  float pad = 0.0f;
  // Colour matrix rows: r' = row_r.xyz . rgb + row_r.w (etc.).
  float row_r[4] = {1, 0, 0, 0};
  float row_g[4] = {0, 1, 0, 0};
  float row_b[4] = {0, 0, 1, 0};
  float shadows[4] = {0, 0, 0, 0.4f};  // rgb offset weighted to dark tones, [3] = contrast pivot
  float highlights[4] = {0, 0, 0, 0};  // rgb offset weighted to bright tones
  float fade[4] = {0, 0, 0, 0};        // rgb colour, [3] = amount
};

}  // namespace rex::ui

extern "C" {
// Copies `grade` (enabled = 0 turns the pass off).
void rex_set_swap_color_grade(const rex::ui::SwapColorGrade* grade);
// Returns true and fills `out` when a grade is enabled.
bool rex_get_swap_color_grade(rex::ui::SwapColorGrade* out);
}
