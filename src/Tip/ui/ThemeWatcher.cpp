#include "ui/ThemeWatcher.h"

#include <cwchar>

#include "Globals.h"

namespace llaime {
namespace {

constexpr wchar_t kClassName[] = L"LLaIME.ThemeWatcher";
bool g_classRegistered = false;

}  // namespace

ThemeWatcher::ThemeWatcher(std::function<void()> onChange) : onChange_(std::move(onChange)) {
  if (!g_classRegistered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &ThemeWatcher::WndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = kClassName;
    g_classRegistered = RegisterClassExW(&wc) != 0;
  }
  if (g_classRegistered) {
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP, 0, 0,
                            0, 0, nullptr, nullptr, g_hInst, this);
  }
}

ThemeWatcher::~ThemeWatcher() {
  if (hwnd_) DestroyWindow(hwnd_);
}

void ThemeWatcher::UnregisterClass() {
  if (g_classRegistered && ::UnregisterClassW(kClassName, g_hInst)) g_classRegistered = false;
}

LRESULT CALLBACK ThemeWatcher::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  } else if (msg == WM_SETTINGCHANGE && lp &&
             std::wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
    auto* self = reinterpret_cast<ThemeWatcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->onChange_) self->onChange_();
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace llaime
