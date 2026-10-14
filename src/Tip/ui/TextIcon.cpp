#include "ui/TextIcon.h"

#include <shellscalingapi.h>

#include <vector>

namespace llaime {

int LangBarIconSize() {
  UINT dpiX = 96, dpiY = 96;
  GetDpiForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dpiX,
                   &dpiY);
  return GetSystemMetricsForDpi(SM_CYSMICON, dpiY);
}

HICON CreateMonochromeTextIcon(std::wstring_view text, int size) {
  // A monochrome icon is one bitmap of height 2*size: the AND mask on top, the XOR mask below.
  // Glyph pixels: AND=0, XOR=0 (black). Background: AND=1, XOR=0 (transparent).
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  ReleaseDC(nullptr, screen);
  HBITMAP mask = CreateBitmap(size, size * 2, 1, 1, nullptr);
  HGDIOBJ oldBitmap = SelectObject(dc, mask);

  RECT top{0, 0, size, size};
  RECT bottom{0, size, size, size * 2};
  FillRect(dc, &top, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  FillRect(dc, &bottom, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

  HFONT font = CreateFontW(-static_cast<int>(size * 0.92), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft JhengHei UI");
  HGDIOBJ oldFont = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(0, 0, 0));
  DrawTextW(dc, text.data(), static_cast<int>(text.size()), &top,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, oldFont);
  DeleteObject(font);
  SelectObject(dc, oldBitmap);
  DeleteDC(dc);

  ICONINFO info{};
  info.fIcon = TRUE;
  info.hbmMask = mask;
  info.hbmColor = nullptr;
  HICON icon = CreateIconIndirect(&info);
  DeleteObject(mask);
  return icon;
}

}  // namespace llaime
