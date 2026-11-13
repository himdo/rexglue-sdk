#include <rex/ui/color_grade.h>

#include <mutex>

namespace {
std::mutex g_grade_mutex;
rex::ui::SwapColorGrade g_grade;
}  // namespace

extern "C" void rex_set_swap_color_grade(const rex::ui::SwapColorGrade* grade) {
  std::lock_guard<std::mutex> lock(g_grade_mutex);
  g_grade = grade ? *grade : rex::ui::SwapColorGrade{};
}

extern "C" bool rex_get_swap_color_grade(rex::ui::SwapColorGrade* out) {
  std::lock_guard<std::mutex> lock(g_grade_mutex);
  if (!g_grade.enabled) return false;
  if (out) *out = g_grade;
  return true;
}
