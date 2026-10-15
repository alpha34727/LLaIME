#include "ui/NoticeWindow.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

#include "Globals.h"
#include "ui/Graphics.h"

namespace llaime {
namespace {

constexpr wchar_t kClassName[] = L"LLaIME.NoticeWindow";
constexpr UINT_PTR kHideTimer = 1;
// DIPs, matching the candidate window's text and padding.
constexpr float kFontSize = 15;
constexpr float kPadX = 12;
constexpr float kHeight = 36;

bool g_classRegistered = false;

}  // namespace

void NoticeWindow::UnregisterClass() {
  if (g_classRegistered && ::UnregisterClassW(kClassName, g_hInst)) g_classRegistered = false;
}

NoticeWindow::~NoticeWindow() {
  if (hwnd_) DestroyWindow(hwnd_);
}

bool NoticeWindow::EnsureWindow() {
  if (hwnd_) return true;
  if (!g_classRegistered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = &NoticeWindow::WndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    g_classRegistered = RegisterClassExW(&wc) != 0;
    if (!g_classRegistered) return false;
  }
  DPI_AWARENESS_CONTEXT old =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                              WS_EX_TRANSPARENT,
                          kClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_hInst,
                          nullptr);
  if (old) SetThreadDpiAwarenessContext(old);
  if (!hwnd_) return false;
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void NoticeWindow::Show(const std::wstring& text, const RECT& anchor) {
  if (text.empty() || !EnsureWindow()) return;
  text_ = text;
  theme_ = Theme::Current();
  ApplyPopupChrome(hwnd_, theme_.dark);

  HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
  UINT dpiX = 96, dpiY = 96;
  GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  dpi_ = dpiX;
  if (!format_ || formatDpi_ != dpi_) {
    format_.Reset();
    DWriteFactory()->CreateTextFormat(L"Microsoft JhengHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      kFontSize, L"zh-TW", &format_);
    if (!format_) return;
    format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    formatDpi_ = dpi_;
  }
  const float s = dpi_ / 96.0f;
  const int w = static_cast<int>(std::ceil((MeasureTextWidth(text_, format_.Get()) + 2 * kPadX) * s));
  const int h = static_cast<int>(std::ceil(kHeight * s));
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(monitor, &mi);
  const RECT& work = mi.rcWork;
  const int gap = static_cast<int>(4 * s);
  int x = anchor.left;
  int y = anchor.bottom + gap;
  if (y + h > work.bottom) y = anchor.top - gap - h;
  x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - w);
  y = std::max(y, static_cast<int>(work.top));
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
  if (target_) target_->Resize(D2D1::SizeU(w, h));
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  InvalidateRect(hwnd_, nullptr, FALSE);
  SetTimer(hwnd_, kHideTimer, kDurationMs, nullptr);
}

void NoticeWindow::Hide() {
  if (!hwnd_) return;
  KillTimer(hwnd_, kHideTimer);
  ShowWindow(hwnd_, SW_HIDE);
}

void NoticeWindow::Paint() {
  PAINTSTRUCT ps;
  BeginPaint(hwnd_, &ps);
  if (!target_) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    D2DFactory()->CreateHwndRenderTarget(
        props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
        &target_);
  }
  if (target_ && format_) {
    target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->BeginDraw();
    target_->Clear(theme_.background);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    target_->CreateSolidColorBrush(theme_.text, &brush);
    const D2D1_SIZE_F size = target_->GetSize();
    DrawColorText(target_.Get(), text_, format_.Get(),
                  D2D1::RectF(kPadX, 0, size.width, size.height), brush.Get());
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) target_.Reset();
  }
  EndPaint(hwnd_, &ps);
}

LRESULT CALLBACK NoticeWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<NoticeWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      if (self) {
        self->Paint();
        return 0;
      }
      break;
    case WM_TIMER:
      if (self && wp == kHideTimer) self->Hide();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace llaime
