#include "ui/CandidateWindow.h"

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
constexpr float kInset = 4;         // gap between window edge and cell highlight
constexpr float kRowHeight = 32;
constexpr float kNumberX = 14;
constexpr float kTextX = 34;        // text start in the first (numbered) column
constexpr float kCellTextPad = 12;  // text start in the other columns
constexpr float kRightColumn = 18;  // page dots in the list layout
constexpr float kFooterHeight = 32;
constexpr float kMinWidth = 150;
constexpr float kMinColumnWidth = 72;
constexpr float kFontSize = 15;
constexpr float kIconSize = 14;
constexpr float kCellRadius = 4;
constexpr float kPillWidth = 3;
constexpr float kPillHeight = 16;
constexpr float kDotRadius = 2;
constexpr float kDotSpacing = 10;
constexpr float kArrowSize = 8;
constexpr float kButtonWidth = 32;
constexpr size_t kMaxDots = 5;
constexpr float kPercentWidth = 36;    // room for "100%" beside the selected candidate
constexpr float kPercentFontSize = 12;
constexpr float kBarHeight = 2;
constexpr float kBarBottom = 5;        // bar baseline above the cell bottom
constexpr float kBarOpacity = 0.85f;

// Segoe Fluent Icons glyphs used by Microsoft Bopomofo's footer.
constexpr wchar_t kGlyphExpand[] = L"\xE89F";
constexpr wchar_t kGlyphCollapse[] = L"\xE8A0";
constexpr wchar_t kGlyphEmoji[] = L"\xF6B8";

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

bool Contains(const D2D1_RECT_F& r, float x, float y) {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

}  // namespace

void CandidateWindow::UnregisterClass() {
  if (g_classRegistered && ::UnregisterClassW(kClassName, g_hInst)) g_classRegistered = false;
}

CandidateWindow::CandidateWindow(Handlers handlers) : handlers_(std::move(handlers)) {}

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
    iconFormat_.Reset();
    percentFormat_.Reset();
    DWriteFactory()->CreateTextFormat(L"Microsoft JhengHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      kFontSize, L"zh-TW", &textFormat_);
    DWriteFactory()->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                                      DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, kFontSize, L"zh-TW",
                                      &numberFormat_);
    DWriteFactory()->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      kIconSize, L"en-US", &iconFormat_);
    DWriteFactory()->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                                      DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, kPercentFontSize, L"en-US",
                                      &percentFormat_);
    if (!textFormat_ || !numberFormat_ || !iconFormat_ || !percentFormat_) return false;
    for (IDWriteTextFormat* f :
         {textFormat_.Get(), numberFormat_.Get(), iconFormat_.Get(), percentFormat_.Get()}) {
      f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    percentFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
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

void CandidateWindow::Show(std::vector<std::wstring> items, const RECT& anchor,
                           bool withProbabilities) {
  if (items.empty() || !EnsureWindow()) {
    Hide();
    return;
  }
  items_ = std::move(items);
  probabilities_.clear();
  withProbabilities_ = withProbabilities;
  selection_ = 0;
  hoverItem_ = -1;
  hoverButton_ = FooterButton::None;
  theme_ = Theme::Current();
  ApplyPopupChrome(hwnd_, theme_.dark);
  UpdateLayout(anchor);
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CandidateWindow::Hide() {
  if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
  items_.clear();
  probabilities_.clear();
}

void CandidateWindow::SetProbabilities(std::vector<double> probabilities) {
  if (items_.empty()) return;
  probabilities_ = std::move(probabilities);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CandidateWindow::SetExpanded(bool expanded) {
  if (expanded_ == expanded) return;
  expanded_ = expanded;
  hoverItem_ = -1;
  if (!items_.empty()) {
    UpdateLayout(anchor_);
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void CandidateWindow::UpdateLayout(const RECT& anchor) {
  anchor_ = anchor;
  HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
  UINT dpiX = 96, dpiY = 96;
  GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  dpi_ = dpiX;
  EnsureDeviceResources();

  // Columns are sized for the widest candidate overall, so paging does not resize the window.
  float maxText = 0;
  for (const auto& item : items_) {
    maxText = std::max(maxText, MeasureTextWidth(item, textFormat_.Get()));
  }
  const size_t rows = std::min(kRows, expanded_ ? kRows : items_.size());
  if (withProbabilities_) maxText += kPercentWidth;
  if (expanded_) {
    firstColumnWidth_ = kTextX - kInset + maxText + 20;
    columnWidth_ = std::max(kMinColumnWidth, maxText + 2 * kCellTextPad);
    widthDip_ = kInset + firstColumnWidth_ + (kExpandedColumns - 1) * columnWidth_ + kInset;
  } else {
    widthDip_ = std::max(kMinWidth, kTextX + maxText + 16 + kRightColumn);
    firstColumnWidth_ = widthDip_ - 2 * kInset;
    columnWidth_ = 0;
  }
  heightDip_ = kInset + rows * kRowHeight + kInset + kFooterHeight;

  const float s = Scale();
  int w = static_cast<int>(std::ceil(widthDip_ * s));
  int h = static_cast<int>(std::ceil(heightDip_ * s));

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

void CandidateWindow::MoveColumn(int delta) {
  if (items_.empty()) return;
  if (!expanded_) {
    // As in Microsoft Bopomofo: Right expands the list; Left keeps paging back.
    if (delta > 0) {
      SetExpanded(true);
    } else {
      MovePage(delta);
    }
    return;
  }
  const size_t start = PageStart();
  const size_t offset = selection_ - start;
  const size_t column = offset / kRows;
  const size_t row = offset % kRows;
  const size_t pageItems = std::min(items_.size() - start, PageSize());
  const size_t lastColumn = (pageItems - 1) / kRows;
  if (delta < 0) {
    if (column == 0) {
      SetExpanded(false);  // Left from the first column collapses the list
      return;
    }
    selection_ -= kRows;
  } else {
    // Right from the last column wraps to the first.
    const size_t next = column == lastColumn ? 0 : column + 1;
    selection_ = start + std::min(next * kRows + row, pageItems - 1);
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CandidateWindow::MovePage(int delta) {
  if (items_.empty()) return;
  long long page = static_cast<long long>(selection_ / PageSize()) + delta;
  page = std::clamp<long long>(page, 0, static_cast<long long>(PageCount()) - 1);
  selection_ = static_cast<size_t>(page) * PageSize();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

std::optional<size_t> CandidateWindow::IndexForDigit(int digit) const {
  if (digit < 1 || digit > static_cast<int>(kRows)) return std::nullopt;
  size_t index = PageStart() + digit - 1;
  if (index >= items_.size()) return std::nullopt;
  return index;
}

D2D1_RECT_F CandidateWindow::CellRect(size_t offset) const {
  const size_t col = offset / kRows;
  const size_t row = offset % kRows;
  const float top = kInset + row * kRowHeight;
  const float left = kInset + (col == 0 ? 0 : firstColumnWidth_ + (col - 1) * columnWidth_);
  const float width = col == 0 ? firstColumnWidth_ : columnWidth_;
  return D2D1::RectF(left, top, left + width, top + kRowHeight);
}

int CandidateWindow::HitTestItem(POINT pt) const {
  const float x = pt.x / Scale(), y = pt.y / Scale();
  const size_t start = PageStart();
  const size_t end = std::min(items_.size(), start + PageSize());
  for (size_t i = start; i < end; ++i) {
    if (Contains(CellRect(i - start), x, y)) return static_cast<int>(i);
  }
  return -1;
}

D2D1_RECT_F CandidateWindow::FooterButtonRect(FooterButton button) const {
  const float top = heightDip_ - kFooterHeight + 4;
  const float bottom = heightDip_ - 4;
  switch (button) {
    case FooterButton::Previous: return D2D1::RectF(4, top, 24, bottom);
    case FooterButton::Next: return D2D1::RectF(24, top, 44, bottom);
    case FooterButton::Expand:
      return D2D1::RectF(widthDip_ - 4 - 2 * kButtonWidth, top, widthDip_ - 4 - kButtonWidth,
                         bottom);
    case FooterButton::Emoji:
      return D2D1::RectF(widthDip_ - 4 - kButtonWidth, top, widthDip_ - 4, bottom);
    default: return D2D1::RectF();
  }
}

CandidateWindow::FooterButton CandidateWindow::HitTestFooter(POINT pt) const {
  const float x = pt.x / Scale(), y = pt.y / Scale();
  for (FooterButton b : {FooterButton::Previous, FooterButton::Next, FooterButton::Expand,
                         FooterButton::Emoji}) {
    if (Contains(FooterButtonRect(b), x, y)) return b;
  }
  return FooterButton::None;
}

void CandidateWindow::Paint() {
  if (!EnsureDeviceResources()) return;
  target_->BeginDraw();
  target_->Clear(theme_.background);

  const D2D1_SIZE_F size = target_->GetSize();
  ComPtr<ID2D1SolidColorBrush> brush;
  target_->CreateSolidColorBrush(theme_.text, &brush);

  const size_t start = PageStart();
  const size_t end = std::min(items_.size(), start + PageSize());
  for (size_t i = start; i < end; ++i) {
    const size_t offset = i - start;
    const D2D1_RECT_F cell = CellRect(offset);
    const bool firstColumn = offset < kRows;
    if (i == selection_ || static_cast<int>(i) == hoverItem_) {
      brush->SetColor(i == selection_ ? theme_.selectedFill : theme_.hoverFill);
      target_->FillRoundedRectangle(D2D1::RoundedRect(cell, kCellRadius, kCellRadius),
                                    brush.Get());
    }
    if (i == selection_) {
      const float mid = (cell.top + cell.bottom) / 2;
      D2D1_RECT_F pill = D2D1::RectF(cell.left, mid - kPillHeight / 2, cell.left + kPillWidth,
                                     mid + kPillHeight / 2);
      brush->SetColor(theme_.accent);
      target_->FillRoundedRectangle(D2D1::RoundedRect(pill, kPillWidth / 2, kPillWidth / 2),
                                    brush.Get());
    }
    float textX = cell.left + kCellTextPad;
    if (firstColumn) {
      wchar_t number[2] = {static_cast<wchar_t>(L'1' + offset), 0};
      brush->SetColor(theme_.secondaryText);
      target_->DrawText(number, 1, numberFormat_.Get(),
                        D2D1::RectF(kNumberX, cell.top, kTextX, cell.bottom), brush.Get());
      textX = kTextX;
    }
    const float textRight = expanded_ ? cell.right : cell.right - kRightColumn;
    brush->SetColor(theme_.text);
    DrawColorText(target_.Get(), items_[i], textFormat_.Get(),
                  D2D1::RectF(textX, cell.top, textRight, cell.bottom), brush.Get());
    PaintProbability(brush.Get(), i, textX, expanded_ ? textRight - kCellTextPad : textRight,
                     cell);
  }

  // List layout: page dots stacked in the right column, centered on the list.
  const size_t pages = PageCount();
  if (!expanded_ && pages > 1) {
    const size_t shown = std::min(pages, kMaxDots);
    const size_t current = std::min(start / PageSize(), shown - 1);
    const float listHeight = std::min(kRows, items_.size()) * kRowHeight;
    const float x = size.width - kRightColumn / 2 - 2;
    float y = kInset + listHeight / 2 - (shown - 1) * kDotSpacing / 2;
    for (size_t p = 0; p < shown; ++p, y += kDotSpacing) {
      brush->SetColor(p == current ? theme_.pageDotActive : theme_.pageDot);
      const float r = p == current ? kDotRadius + 0.5f : kDotRadius;
      target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), brush.Get());
    }
  }

  PaintFooter(brush.Get(), size.width, size.height);
  if (target_->EndDraw() == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

void CandidateWindow::PaintProbability(ID2D1SolidColorBrush* brush, size_t index, float textX,
                                       float textRight, const D2D1_RECT_F& cell) {
  const double p = ProbabilityOf(index);
  if (p < 0) return;
  // The bar's full length (100%) is the text area left of the percentage.
  const float full = std::max(0.0f, textRight - kPercentWidth - textX);
  const float length = static_cast<float>(std::clamp(p, 0.0, 1.0)) * full;
  if (length >= 0.5f) {
    const float bottom = cell.bottom - kBarBottom;
    brush->SetColor(theme_.accent);
    brush->SetOpacity(kBarOpacity);
    target_->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(textX, bottom - kBarHeight, textX + length, bottom),
                          kBarHeight / 2, kBarHeight / 2),
        brush);
    brush->SetOpacity(1.0f);
  }
  if (index != selection_) return;
  wchar_t text[8];
  const int percent = static_cast<int>(std::lround(p * 100));
  if (percent < 1) {
    wcscpy_s(text, L"<1%");
  } else {
    swprintf_s(text, L"%d%%", percent);
  }
  brush->SetColor(theme_.secondaryText);
  target_->DrawText(text, static_cast<UINT32>(wcslen(text)), percentFormat_.Get(),
                    D2D1::RectF(textRight - kPercentWidth, cell.top, textRight, cell.bottom),
                    brush);
}

void CandidateWindow::PaintFooter(ID2D1SolidColorBrush* brush, float width, float height) {
  const float footerTop = height - kFooterHeight;
  const float footerMid = footerTop + kFooterHeight / 2;
  brush->SetColor(theme_.separator);
  target_->DrawLine(D2D1::Point2F(0, footerTop), D2D1::Point2F(width, footerTop), brush,
                    1.0f / Scale());

  for (FooterButton b : {FooterButton::Previous, FooterButton::Next, FooterButton::Expand,
                         FooterButton::Emoji}) {
    if (b == hoverButton_) {
      brush->SetColor(theme_.hoverFill);
      target_->FillRoundedRectangle(
          D2D1::RoundedRect(FooterButtonRect(b), kCellRadius, kCellRadius), brush);
    }
  }

  // Paging arrows: ▲▼ for the list, ◀▶ when expanded.
  const size_t start = PageStart();
  const bool canPrev = start > 0;
  const bool canNext = start + PageSize() < items_.size();
  auto drawArrow = [&](float cx, int direction, bool enabled) {
    // direction: 0 up, 1 down, 2 left, 3 right
    ComPtr<ID2D1PathGeometry> geo;
    D2DFactory()->CreatePathGeometry(&geo);
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    const float along = kArrowSize * 0.6f / 2, across = kArrowSize / 2;
    D2D1_POINT_2F tip, a, b;
    switch (direction) {
      case 0: tip = {cx, footerMid - along}; a = {cx + across, footerMid + along};
              b = {cx - across, footerMid + along}; break;
      case 1: tip = {cx, footerMid + along}; a = {cx + across, footerMid - along};
              b = {cx - across, footerMid - along}; break;
      case 2: tip = {cx - along, footerMid}; a = {cx + along, footerMid - across};
              b = {cx + along, footerMid + across}; break;
      default: tip = {cx + along, footerMid}; a = {cx - along, footerMid - across};
               b = {cx - along, footerMid + across}; break;
    }
    sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(a);
    sink->AddLine(b);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    brush->SetColor(enabled ? theme_.text : theme_.pageDot);
    target_->FillGeometry(geo.Get(), brush);
  };
  drawArrow(14, expanded_ ? 2 : 0, canPrev);
  drawArrow(34, expanded_ ? 3 : 1, canNext);

  // Expanded layout: page dots centered in the footer.
  const size_t pages = PageCount();
  if (expanded_ && pages > 1) {
    const size_t shown = std::min(pages, kMaxDots);
    const size_t current = std::min(start / PageSize(), shown - 1);
    float x = width / 2 - (shown - 1) * kDotSpacing / 2;
    for (size_t p = 0; p < shown; ++p, x += kDotSpacing) {
      brush->SetColor(p == current ? theme_.pageDotActive : theme_.pageDot);
      const float r = p == current ? kDotRadius + 0.5f : kDotRadius;
      target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, footerMid), r, r), brush);
    }
  }

  brush->SetColor(theme_.text);
  target_->DrawText(expanded_ ? kGlyphCollapse : kGlyphExpand, 1, iconFormat_.Get(),
                    FooterButtonRect(FooterButton::Expand), brush);
  target_->DrawText(kGlyphEmoji, 1, iconFormat_.Get(), FooterButtonRect(FooterButton::Emoji),
                    brush);
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
      const int item = HitTestItem(pt);
      const FooterButton button = HitTestFooter(pt);
      if (item != hoverItem_ || button != hoverButton_) {
        hoverItem_ = item;
        hoverButton_ = button;
        InvalidateRect(hwnd_, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      hoverItem_ = -1;
      hoverButton_ = FooterButton::None;
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      const int item = HitTestItem(pt);
      if (item >= 0) {
        if (handlers_.select) handlers_.select(static_cast<size_t>(item));
        return 0;
      }
      switch (HitTestFooter(pt)) {
        case FooterButton::Previous: MovePage(-1); break;
        case FooterButton::Next: MovePage(1); break;
        case FooterButton::Expand: SetExpanded(!expanded_); break;
        case FooterButton::Emoji:
          if (handlers_.emoji) handlers_.emoji();
          break;
        default: break;
      }
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
