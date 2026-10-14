#include "ui/CandidateWindow.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "Globals.h"
#include "ui/Graphics.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

constexpr wchar_t kClassName[] = L"LLaIME.CandidateWindow";

// Layout metrics in DIPs, measured from the Windows 11 Microsoft Bopomofo window.
constexpr float kInset = 4;         // gap between window edge and row highlight
constexpr float kRowHeight = 32;
constexpr float kNumberX = 14;
constexpr float kTextX = 34;
constexpr float kRightColumn = 18;  // page dots
constexpr float kFooterHeight = 32;
constexpr float kMinWidth = 150;
constexpr float kFontSize = 15;
constexpr float kRowRadius = 4;
constexpr float kPillWidth = 3;
constexpr float kPillHeight = 16;
constexpr float kDotRadius = 2;
constexpr float kDotSpacing = 10;
constexpr float kArrowSize = 8;

bool g_classRegistered = false;

bool RegisterWindowClass(WNDPROC proc) {
  if (!g_classRegistered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = proc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    g_classRegistered = RegisterClassExW(&wc) != 0;
  }
  return g_classRegistered;
}

}  // namespace

void CandidateWindow::UnregisterClass() {
  if (g_classRegistered && ::UnregisterClassW(kClassName, g_hInst)) g_classRegistered = false;
}

CandidateWindow::CandidateWindow(SelectHandler onSelect) : onSelect_(std::move(onSelect)) {}

CandidateWindow::~CandidateWindow() {
  if (hwnd_) DestroyWindow(hwnd_);
}

bool CandidateWindow::EnsureWindow() {
  if (hwnd_) return true;
  if (!RegisterWindowClass(&CandidateWindow::WndProc)) return false;
  // Per-monitor aware regardless of the host app, so text stays crisp at any scale.
  DPI_AWARENESS_CONTEXT old =
      SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName, L"",
                          WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_hInst, nullptr);
  if (old) SetThreadDpiAwarenessContext(old);
  if (!hwnd_) return false;
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

bool CandidateWindow::EnsureDeviceResources() {
  if (!textFormat_ || formatDpi_ != dpi_) {
    // Text formats are created in DIPs; the render target applies the DPI scale.
    textFormat_.Reset();
    numberFormat_.Reset();
    DWriteFactory()->CreateTextFormat(L"Microsoft JhengHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      kFontSize, L"zh-TW", &textFormat_);
    DWriteFactory()->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                                      DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, kFontSize, L"zh-TW",
                                      &numberFormat_);
    if (!textFormat_ || !numberFormat_) return false;
    for (IDWriteTextFormat* f : {textFormat_.Get(), numberFormat_.Get()}) {
      f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    formatDpi_ = dpi_;
  }
  if (!target_) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        static_cast<float>(dpi_), static_cast<float>(dpi_));
    if (FAILED(D2DFactory()->CreateHwndRenderTarget(
            props,
            D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right - rc.left,
                                                                rc.bottom - rc.top)),
            &target_))) {
      return false;
    }
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  }
  target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
  return true;
}

void CandidateWindow::DiscardDeviceResources() { target_.Reset(); }

void CandidateWindow::Show(std::vector<std::wstring> items, const RECT& anchor) {
  if (items.empty() || !EnsureWindow()) {
    Hide();
    return;
  }
  items_ = std::move(items);
  selection_ = 0;
  hoverRow_ = -1;
  theme_ = Theme::Current();
  ApplyPopupChrome(hwnd_, theme_.dark);
  UpdateLayout(anchor);
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CandidateWindow::Hide() {
  if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
  items_.clear();
}

void CandidateWindow::UpdateLayout(const RECT& anchor) {
  anchor_ = anchor;
  HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
  UINT dpiX = 96, dpiY = 96;
  GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  dpi_ = dpiX;
  EnsureDeviceResources();

  float maxText = 0;
  for (const auto& item : items_) maxText = std::max(maxText, MeasureTextWidth(item, textFormat_.Get()));
  widthDip_ = std::max(kMinWidth, kTextX + maxText + 16 + kRightColumn);
  const size_t rows = std::min(kPageSize, items_.size());
  const float heightDip = kInset + rows * kRowHeight + kInset + kFooterHeight;

  const float s = Scale();
  int w = static_cast<int>(std::ceil(widthDip_ * s));
  int h = static_cast<int>(std::ceil(heightDip * s));

  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(monitor, &mi);
  const RECT& work = mi.rcWork;
  int gap = static_cast<int>(4 * s);
  int x = anchor.left;
  int y = anchor.bottom + gap;
  if (y + h > work.bottom) y = anchor.top - gap - h;  // flip above the text
  x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - w);
  y = std::max(y, static_cast<int>(work.top));

  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
  if (target_) target_->Resize(D2D1::SizeU(w, h));
}

void CandidateWindow::MoveSelection(int delta) {
  if (items_.empty()) return;
  long long next = static_cast<long long>(selection_) + delta;
  selection_ = static_cast<size_t>(std::clamp<long long>(next, 0, items_.size() - 1));
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CandidateWindow::MovePage(int delta) {
  if (items_.empty()) return;
  long long page = static_cast<long long>(selection_ / kPageSize) + delta;
  page = std::clamp<long long>(page, 0, static_cast<long long>(PageCount()) - 1);
  selection_ = static_cast<size_t>(page) * kPageSize;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

std::optional<size_t> CandidateWindow::IndexForDigit(int digit) const {
  if (digit < 1 || digit > static_cast<int>(kPageSize)) return std::nullopt;
  size_t index = PageStart() + digit - 1;
  if (index >= items_.size()) return std::nullopt;
  return index;
}

int CandidateWindow::HitTestRow(POINT pt) const {
  float y = pt.y / Scale() - kInset;
  float x = pt.x / Scale();
  if (y < 0 || x < kInset || x > widthDip_ - kInset) return -1;
  int row = static_cast<int>(y / kRowHeight);
  size_t index = PageStart() + row;
  if (row >= static_cast<int>(kPageSize) || index >= items_.size()) return -1;
  return row;
}

int CandidateWindow::HitTestFooter(POINT pt) const {
  RECT rc;
  GetClientRect(hwnd_, &rc);
  float y = pt.y / Scale();
  float x = pt.x / Scale();
  float footerTop = rc.bottom / Scale() - kFooterHeight;
  if (y < footerTop) return 0;
  if (x >= 4 && x < 24) return -1;
  if (x >= 24 && x < 44) return 1;
  return 0;
}

void CandidateWindow::Paint() {
  if (!EnsureDeviceResources()) return;
  target_->BeginDraw();
  target_->Clear(theme_.background);

  D2D1_SIZE_F size = target_->GetSize();
  ComPtr<ID2D1SolidColorBrush> brush;
  target_->CreateSolidColorBrush(theme_.text, &brush);

  const size_t start = PageStart();
  const size_t end = std::min(items_.size(), start + kPageSize);
  for (size_t i = start; i < end; ++i) {
    const float top = kInset + (i - start) * kRowHeight;
    D2D1_RECT_F row = D2D1::RectF(kInset, top, size.width - kInset, top + kRowHeight);
    if (i == selection_ || static_cast<int>(i - start) == hoverRow_) {
      brush->SetColor(i == selection_ ? theme_.selectedFill : theme_.hoverFill);
      target_->FillRoundedRectangle(D2D1::RoundedRect(row, kRowRadius, kRowRadius), brush.Get());
    }
    if (i == selection_) {
      float mid = top + kRowHeight / 2;
      D2D1_RECT_F pill = D2D1::RectF(kInset, mid - kPillHeight / 2, kInset + kPillWidth,
                                     mid + kPillHeight / 2);
      brush->SetColor(theme_.accent);
      target_->FillRoundedRectangle(D2D1::RoundedRect(pill, kPillWidth / 2, kPillWidth / 2),
                                    brush.Get());
    }
    wchar_t number[2] = {static_cast<wchar_t>(L'1' + (i - start)), 0};
    brush->SetColor(theme_.secondaryText);
    target_->DrawText(number, 1, numberFormat_.Get(),
                      D2D1::RectF(kNumberX, top, kTextX, top + kRowHeight), brush.Get());
    brush->SetColor(theme_.text);
    const std::wstring& text = items_[i];
    DrawColorText(target_.Get(), text, textFormat_.Get(),
                  D2D1::RectF(kTextX, top, row.right - kRightColumn, top + kRowHeight),
                  brush.Get());
  }

  // Page dots, vertically centered in the list area.
  const size_t pages = PageCount();
  if (pages > 1) {
    const size_t shown = std::min<size_t>(pages, 5);
    const size_t current = std::min(start / kPageSize, shown - 1);
    const float listHeight = std::min(kPageSize, items_.size()) * kRowHeight;
    const float x = size.width - kRightColumn / 2 - 2;
    float y = kInset + listHeight / 2 - (shown - 1) * kDotSpacing / 2;
    for (size_t p = 0; p < shown; ++p, y += kDotSpacing) {
      brush->SetColor(p == current ? theme_.pageDotActive : theme_.pageDot);
      float r = p == current ? kDotRadius + 0.5f : kDotRadius;
      target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), brush.Get());
    }
  }

  // Footer: separator and paging arrows.
  const float footerTop = size.height - kFooterHeight;
  brush->SetColor(theme_.separator);
  target_->DrawLine(D2D1::Point2F(0, footerTop), D2D1::Point2F(size.width, footerTop), brush.Get(),
                    1.0f / Scale());
  auto drawTriangle = [&](float cx, float cy, bool up, bool enabled) {
    ComPtr<ID2D1PathGeometry> geo;
    D2DFactory()->CreatePathGeometry(&geo);
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    float h = kArrowSize * 0.6f, w = kArrowSize / 2;
    float tipY = up ? cy - h / 2 : cy + h / 2;
    float baseY = up ? cy + h / 2 : cy - h / 2;
    sink->BeginFigure(D2D1::Point2F(cx, tipY), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(cx + w, baseY));
    sink->AddLine(D2D1::Point2F(cx - w, baseY));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    brush->SetColor(enabled ? theme_.text : theme_.pageDot);
    target_->FillGeometry(geo.Get(), brush.Get());
  };
  const float footerMid = footerTop + kFooterHeight / 2;
  drawTriangle(14, footerMid, true, start > 0);
  drawTriangle(34, footerMid, false, end < items_.size());

  if (target_->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

LRESULT CALLBACK CandidateWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self) return self->HandleMessage(msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CandidateWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd_, &ps);
      Paint();
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_ARROW));
      return TRUE;
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      int row = HitTestRow(pt);
      if (row != hoverRow_) {
        hoverRow_ = row;
        InvalidateRect(hwnd_, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      hoverRow_ = -1;
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      int row = HitTestRow(pt);
      if (row >= 0) {
        size_t index = PageStart() + row;
        if (onSelect_) onSelect_(index);
        return 0;
      }
      if (int dir = HitTestFooter(pt)) MovePage(dir);
      return 0;
    }
    case WM_MOUSEWHEEL:
      MovePage(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
      return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
      if (!items_.empty()) UpdateLayout(anchor_);
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace llaime
