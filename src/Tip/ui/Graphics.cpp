#include "ui/Graphics.h"

#include <d2d1_1.h>
#include <dwmapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace llaime {

ID2D1Factory* D2DFactory() {
  static ComPtr<ID2D1Factory> factory = [] {
    ComPtr<ID2D1Factory> f;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf());
    return f;
  }();
  return factory.Get();
}

IDWriteFactory* DWriteFactory() {
  static ComPtr<IDWriteFactory> factory = [] {
    ComPtr<IDWriteFactory> f;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(f.GetAddressOf()));
    return f;
  }();
  return factory.Get();
}

void DrawColorText(ID2D1RenderTarget* target, const std::wstring& text, IDWriteTextFormat* format,
                   const D2D1_RECT_F& rect, ID2D1Brush* brush) {
  target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                   D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float MeasureTextWidth(const std::wstring& text, IDWriteTextFormat* format) {
  ComPtr<IDWriteTextLayout> layout;
  if (!format || FAILED(DWriteFactory()->CreateTextLayout(
                     text.c_str(), static_cast<UINT32>(text.size()), format, 4096, 100, &layout))) {
    return 0;
  }
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  return m.widthIncludingTrailingWhitespace;
}

void ApplyPopupChrome(HWND hwnd, bool dark) {
  DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
  BOOL useDark = dark;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
}

}  // namespace llaime
