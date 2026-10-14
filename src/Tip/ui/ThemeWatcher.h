#pragma once
#include <windows.h>

#include <functional>

namespace llaime {

// Invisible top-level window that calls `onChange` when the Windows light/dark theme changes
// (WM_SETTINGCHANGE "ImmersiveColorSet"; message-only windows do not receive broadcasts).
class ThemeWatcher {
 public:
  explicit ThemeWatcher(std::function<void()> onChange);
  ~ThemeWatcher();
  ThemeWatcher(const ThemeWatcher&) = delete;
  ThemeWatcher& operator=(const ThemeWatcher&) = delete;

  static void UnregisterClass();

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  std::function<void()> onChange_;
  HWND hwnd_ = nullptr;
};

}  // namespace llaime
