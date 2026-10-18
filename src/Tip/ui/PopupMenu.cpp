#include "ui/PopupMenu.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Globals.h"
#include "Guard.h"
#include "ui/Graphics.h"
#include "ui/Theme.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

constexpr wchar_t kClassName[] = L"LLaIME.PopupMenu";

// Metrics in DIPs, following the Windows 11 taskbar context menus.
constexpr float kPadding = 4;
constexpr float kItemHeight = 32;
constexpr float kSeparatorHeight = 9;
constexpr float kCheckX = 14;
constexpr float kTextX = 44;
constexpr float kRightPadding = 28;
constexpr float kMinWidth = 180;
constexpr float kFontSize = 14;
constexpr float kRadius = 4;
constexpr float kGapAboveTaskbar = 8;
constexpr UINT_PTR kPollTimer = 1;
// Opening animation: the menu rises out of the taskbar, decelerating (Windows 11 flyouts).
constexpr double kOpenAnimationMs = 220;

bool g_classRegistered = false;

class PopupMenu {
 public:
  explicit PopupMenu(const std::vector<PopupMenuItem>& items)
      : items_(items), theme_(Theme::ForSystem()) {}

  UINT Run();

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
  float Scale() const { return dpi_ / 96.0f; }
  float ItemTop(size_t index) const;
  float ContentHeight() const { return ItemTop(items_.size()) + kPadding; }
  int HitTest(POINT pt) const;
  void Paint();
  void Poll();
  // Advances the opening animation; returns false once it has finished.
  bool AnimateStep();
  void Finish(UINT result) {
    result_ = result;
    done_ = true;
  }

  const std::vector<PopupMenuItem>& items_;
  Theme theme_;
  HWND hwnd_ = nullptr;
  UINT dpi_ = 96;
  float width_ = kMinWidth;
  int hover_ = -1;
  bool done_ = false;
  UINT result_ = 0;
  bool buttonWasDown_[3] = {};
  bool escapeWasDown_ = false;
  int x_ = 0, bottom_ = 0, w_ = 0, h_ = 0;  // final window rect (physical pixels)
  std::chrono::steady_clock::time_point animationStart_;
  ComPtr<ID2D1HwndRenderTarget> target_;
  ComPtr<IDWriteTextFormat> textFormat_;
  ComPtr<IDWriteTextFormat> glyphFormat_;
};

float PopupMenu::ItemTop(size_t index) const {
  float y = kPadding;
  for (size_t i = 0; i < index && i < items_.size(); ++i) {
    y += items_[i].separator ? kSeparatorHeight : kItemHeight;
  }
  return y;
}

int PopupMenu::HitTest(POINT pt) const {
  float y = pt.y / Scale();
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].separator) continue;
    float top = ItemTop(i);
    if (y >= top && y < top + kItemHeight) return static_cast<int>(i);
  }
  return -1;
}

UINT PopupMenu::Run() {
  if (!g_classRegistered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = &PopupMenu::WndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    g_classRegistered = RegisterClassExW(&wc) != 0;
    if (!g_classRegistered) return 0;
  }

  // Work in physical pixels regardless of the host app's DPI awareness.
  DPI_AWARENESS_CONTEXT oldContext =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  POINT cursor;
  GetCursorPos(&cursor);
  HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  UINT dpiY = 96;
  GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_, &dpiY);

  DWriteFactory()->CreateTextFormat(L"Microsoft JhengHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    kFontSize, L"zh-TW", &textFormat_);
  DWriteFactory()->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12,
                                    L"en-US", &glyphFormat_);
  if (!textFormat_ || !glyphFormat_) {
    if (oldContext) SetThreadDpiAwarenessContext(oldContext);
    return 0;
  }
  for (IDWriteTextFormat* f : {textFormat_.Get(), glyphFormat_.Get()}) {
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  }
  float maxText = 0;
  for (const auto& item : items_) maxText = std::max(maxText, MeasureTextWidth(item.text, textFormat_.Get()));
  width_ = std::max(kMinWidth, kTextX + maxText + kRightPadding);

  const int w = static_cast<int>(std::ceil(width_ * Scale()));
  const int h = static_cast<int>(std::ceil(ContentHeight() * Scale()));
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(monitor, &mi);
  const RECT& work = mi.rcWork;
  const int gap = static_cast<int>(kGapAboveTaskbar * Scale());
  // Opened from the taskbar: sit just above it, centered on the cursor.
  int bottom = cursor.y >= work.bottom ? work.bottom - gap : cursor.y;
  int x = std::clamp(static_cast<int>(cursor.x - w / 2), static_cast<int>(work.left) + gap,
                     static_cast<int>(work.right) - w - gap);
  int y = std::max(static_cast<int>(work.top), bottom - h);

  x_ = x;
  bottom_ = y + h;
  w_ = w;
  h_ = h;
  // Start as a one-pixel strip at the bottom edge; AnimateStep grows it upward.
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName, L"",
                          WS_POPUP, x, bottom_ - 1, w, 1, nullptr, nullptr, g_hInst, this);
  if (oldContext) SetThreadDpiAwarenessContext(oldContext);
  if (!hwnd_) return 0;
  ApplyPopupChrome(hwnd_, theme_.dark);

  buttonWasDown_[0] = GetAsyncKeyState(VK_LBUTTON) & 0x8000;
  buttonWasDown_[1] = GetAsyncKeyState(VK_RBUTTON) & 0x8000;
  buttonWasDown_[2] = GetAsyncKeyState(VK_MBUTTON) & 0x8000;
  escapeWasDown_ = GetAsyncKeyState(VK_ESCAPE) & 0x8000;
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  SetTimer(hwnd_, kPollTimer, 20, nullptr);
  animationStart_ = std::chrono::steady_clock::now();
  bool animating = true;

  // Modal loop like TrackPopupMenu. Mouse clicks elsewhere go to other threads' windows, so
  // dismissal is detected by polling button state rather than by capture.
  MSG msg;
  bool quit = false;
  while (!done_ && !quit) {
    if (animating) {
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
          PostQuitMessage(static_cast<int>(msg.wParam));
          quit = true;
          break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (!quit && !done_) {
        animating = AnimateStep();
        DwmFlush();  // pace frames to the display refresh
      }
      continue;
    }
    BOOL r = GetMessageW(&msg, nullptr, 0, 0);
    if (r == 0) {
      PostQuitMessage(static_cast<int>(msg.wParam));
      break;
    }
    if (r == -1) break;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  KillTimer(hwnd_, kPollTimer);
  DestroyWindow(hwnd_);
  hwnd_ = nullptr;
  return result_;
}

bool PopupMenu::AnimateStep() {
  using namespace std::chrono;
  const double t = std::min(
      1.0, duration<double, std::milli>(steady_clock::now() - animationStart_).count() /
               kOpenAnimationMs);
  const double eased = 1.0 - std::pow(1.0 - t, 3.0);  // ease-out cubic
  const int visible = std::max(1, static_cast<int>(std::lround(h_ * eased)));
  SetWindowPos(hwnd_, nullptr, x_, bottom_ - visible, w_, visible,
               SWP_NOACTIVATE | SWP_NOZORDER);
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  return t < 1.0;
}

void PopupMenu::Poll() {
  POINT pt;
  GetCursorPos(&pt);
  RECT rc;
  GetWindowRect(hwnd_, &rc);
  const int keys[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
  for (int i = 0; i < 3; ++i) {
    bool down = GetAsyncKeyState(keys[i]) & 0x8000;
    if (down && !buttonWasDown_[i] && !PtInRect(&rc, pt)) Finish(0);
    buttonWasDown_[i] = down;
  }
  bool escape = GetAsyncKeyState(VK_ESCAPE) & 0x8000;
  if (escape && !escapeWasDown_) Finish(0);
  escapeWasDown_ = escape;
}

void PopupMenu::Paint() {
  if (!target_) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        static_cast<float>(dpi_), static_cast<float>(dpi_));
    if (FAILED(D2DFactory()->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
            &target_))) {
      return;
    }
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  }
  target_->BeginDraw();
  target_->Clear(theme_.background);
  ComPtr<ID2D1SolidColorBrush> brush;
  target_->CreateSolidColorBrush(theme_.text, &brush);
  const float width = target_->GetSize().width;

  for (size_t i = 0; i < items_.size(); ++i) {
    const PopupMenuItem& item = items_[i];
    const float top = ItemTop(i);
    if (item.separator) {
      const float y = top + kSeparatorHeight / 2;
      brush->SetColor(theme_.separator);
      target_->DrawLine(D2D1::Point2F(0, y), D2D1::Point2F(width, y), brush.Get(), 1.0f / Scale());
      continue;
    }
    if (static_cast<int>(i) == hover_ && item.enabled) {
      brush->SetColor(theme_.hoverFill);
      D2D1_RECT_F r = D2D1::RectF(kPadding, top, width - kPadding, top + kItemHeight);
      target_->FillRoundedRectangle(D2D1::RoundedRect(r, kRadius, kRadius), brush.Get());
    }
    brush->SetColor(item.enabled ? theme_.text : theme_.secondaryText);
    if (item.checked) {
      // Segoe Fluent Icons "CheckMark".
      target_->DrawText(L"\xE73E", 1, glyphFormat_.Get(),
                        D2D1::RectF(kCheckX, top, kTextX, top + kItemHeight), brush.Get());
    }
    DrawColorText(target_.Get(), item.text, textFormat_.Get(),
                  D2D1::RectF(kTextX, top, width - kPadding, top + kItemHeight), brush.Get());
  }
  if (target_->EndDraw() == D2DERR_RECREATE_TARGET) target_.Reset();
}

LRESULT CALLBACK PopupMenu::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  return Guarded("PopupMenu::WndProc", LRESULT(0), [&]() -> LRESULT {
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      reinterpret_cast<PopupMenu*>(cs->lpCreateParams)->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<PopupMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
  });
}

LRESULT PopupMenu::Handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd_, &ps);
      Paint();
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_TIMER:
      if (wp == kPollTimer) Poll();
      return 0;
    case WM_SIZE:
      if (target_) target_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
      return 0;
    case WM_MOUSEMOVE: {
      int hit = HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
      if (hit != hover_) {
        hover_ = hit;
        InvalidateRect(hwnd_, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      hover_ = -1;
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
      int hit = HitTest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
      if (hit >= 0 && items_[hit].enabled) Finish(items_[hit].id);
      return 0;
    }
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace

UINT ShowPopupMenu(const std::vector<PopupMenuItem>& items) {
  PopupMenu menu(items);
  return menu.Run();
}

void UnregisterPopupMenuClass() {
  if (g_classRegistered && UnregisterClassW(kClassName, g_hInst)) g_classRegistered = false;
}

}  // namespace llaime
