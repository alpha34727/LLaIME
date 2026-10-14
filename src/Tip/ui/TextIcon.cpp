#include "ui/TextIcon.h"

#include <d2d1_1.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>

#include "ui/Graphics.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

// Fraction of the icon the glyph's ink spans, matched to Microsoft Bopomofo's 中/英/ㄅ.
constexpr float kGlyphInk = 0.88f;
// Shield badge: bottom-right square, as a fraction of the icon size.
constexpr float kShieldSize = 0.5f;

ComPtr<ID2D1PathGeometry> ShieldGeometry(float left, float top, float s) {
  ComPtr<ID2D1PathGeometry> geo;
  D2DFactory()->CreatePathGeometry(&geo);
  ComPtr<ID2D1GeometrySink> sink;
  geo->Open(&sink);
  auto P = [&](float x, float y) { return D2D1::Point2F(left + x * s, top + y * s); };
  // Flat top with shoulders, straight sides, curved sides meeting at a bottom point.
  sink->BeginFigure(P(0.50f, 0.04f), D2D1_FIGURE_BEGIN_FILLED);
  sink->AddBezier(D2D1::BezierSegment(P(0.66f, 0.14f), P(0.82f, 0.18f), P(0.94f, 0.18f)));
  sink->AddLine(P(0.94f, 0.48f));
  sink->AddBezier(D2D1::BezierSegment(P(0.94f, 0.74f), P(0.74f, 0.90f), P(0.50f, 0.98f)));
  sink->AddBezier(D2D1::BezierSegment(P(0.26f, 0.90f), P(0.06f, 0.74f), P(0.06f, 0.48f)));
  sink->AddLine(P(0.06f, 0.18f));
  sink->AddBezier(D2D1::BezierSegment(P(0.18f, 0.18f), P(0.34f, 0.14f), P(0.50f, 0.04f)));
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
  sink->Close();
  return geo;
}

}  // namespace

int LangBarIconSize() {
  UINT dpiX = 96, dpiY = 96;
  GetDpiForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dpiX,
                   &dpiY);
  return GetSystemMetricsForDpi(SM_CYSMICON, dpiY);
}

bool SystemUsesLightTheme() {
  DWORD value = 0, size = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  return value != 0;
}

HICON CreateTaskbarTextIcon(std::wstring_view text, int size, bool shield) {
  // Render coverage with Direct2D into a WIC bitmap, then build a straight-alpha icon whose
  // color is the taskbar text color and whose alpha is that coverage.
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&wic)))) {
    return nullptr;
  }
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                               &bitmap))) {
    return nullptr;
  }
  ComPtr<ID2D1RenderTarget> target;
  auto props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
  if (FAILED(D2DFactory()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target))) {
    return nullptr;
  }

  // Lay out at a nominal size, then scale so the glyph's ink box fills kGlyphInk of the icon,
  // centered on the ink rather than on the line box (CJK fonts carry extra ascent/descent).
  const std::wstring str(text);
  const float nominal = 100.0f;
  ComPtr<IDWriteTextFormat> format;
  DWriteFactory()->CreateTextFormat(L"Microsoft JhengHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, nominal,
                                    L"zh-TW", &format);
  ComPtr<IDWriteTextLayout> layout;
  if (!format || FAILED(DWriteFactory()->CreateTextLayout(str.c_str(),
                                                          static_cast<UINT32>(str.size()),
                                                          format.Get(), 0, 0, &layout))) {
    return nullptr;
  }
  layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  DWRITE_OVERHANG_METRICS over{};
  layout->GetOverhangMetrics(&over);
  // With a 0x0 layout box, the overhangs are the ink extents relative to the origin.
  const float inkLeft = -over.left, inkTop = -over.top;
  const float inkWidth = over.right + over.left, inkHeight = over.bottom + over.top;
  const float scale = kGlyphInk * size / std::max(inkWidth, inkHeight);
  const float ox = (size - inkWidth * scale) / 2 - inkLeft * scale;
  const float oy = (size - inkHeight * scale) / 2 - inkTop * scale;

  ComPtr<ID2D1SolidColorBrush> brush;
  target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
  target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  target->BeginDraw();
  target->Clear(D2D1::ColorF(0, 0, 0, 0));
  target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                       D2D1::Matrix3x2F::Translation(ox, oy));
  target->DrawTextLayout(D2D1::Point2F(0, 0), layout.Get(), brush.Get());
  target->SetTransform(D2D1::Matrix3x2F::Identity());

  if (shield) {
    const float s = size * kShieldSize;
    const float left = size - s, top = size - s;
    auto geo = ShieldGeometry(left, top, s);
    // Punch a transparent gap around the badge so it reads separately from the glyph.
    ComPtr<ID2D1DeviceContext> dc;
    if (SUCCEEDED(target.As(&dc))) {
      dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
      brush->SetColor(D2D1::ColorF(0, 0, 0, 0));
      dc->DrawGeometry(geo.Get(), brush.Get(), std::max(1.5f, size / 12.0f));
      dc->FillGeometry(geo.Get(), brush.Get());
      dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    }
    brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
    target->FillGeometry(geo.Get(), brush.Get());
  }
  if (FAILED(target->EndDraw())) return nullptr;

  BITMAPV5HEADER bh{};
  bh.bV5Size = sizeof(bh);
  bh.bV5Width = size;
  bh.bV5Height = -size;
  bh.bV5Planes = 1;
  bh.bV5BitCount = 32;
  bh.bV5Compression = BI_BITFIELDS;
  bh.bV5RedMask = 0x00ff0000;
  bh.bV5GreenMask = 0x0000ff00;
  bh.bV5BlueMask = 0x000000ff;
  bh.bV5AlphaMask = 0xff000000;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS,
                                   &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!color) return nullptr;
  const UINT stride = size * 4;
  bitmap->CopyPixels(nullptr, stride, stride * size, static_cast<BYTE*>(bits));
  const BYTE ink = SystemUsesLightTheme() ? 0x00 : 0xff;
  auto* px = static_cast<BYTE*>(bits);
  for (int i = 0; i < size * size; ++i) {
    BYTE coverage = px[i * 4 + 3];
    px[i * 4 + 0] = ink;
    px[i * 4 + 1] = ink;
    px[i * 4 + 2] = ink;
    px[i * 4 + 3] = coverage;
  }

  HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO info{};
  info.fIcon = TRUE;
  info.hbmMask = mask;
  info.hbmColor = color;
  HICON icon = CreateIconIndirect(&info);
  DeleteObject(mask);
  DeleteObject(color);
  return icon;
}

}  // namespace llaime
