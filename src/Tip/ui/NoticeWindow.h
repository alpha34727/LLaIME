#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

#include "ui/Theme.h"

namespace llaime {

// A one-line message under the text being typed (e.g. "已加入「柏昀」"), styled like the
// candidate window, that fades after a moment. Never takes focus.
class NoticeWindow {
 public:
  NoticeWindow() = default;
  ~NoticeWindow();
  NoticeWindow(const NoticeWindow&) = delete;
  NoticeWindow& operator=(const NoticeWindow&) = delete;

  static constexpr UINT kDurationMs = 2000;

  // `anchor`: screen rectangle of the text (physical pixels).
  void Show(const std::wstring& text, const RECT& anchor);
  void Hide();

  static void UnregisterClass();

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  bool EnsureWindow();
  void Paint();

  HWND hwnd_ = nullptr;
  UINT dpi_ = 96;
  Theme theme_;
  std::wstring text_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
  UINT formatDpi_ = 0;
};

}  // namespace llaime
