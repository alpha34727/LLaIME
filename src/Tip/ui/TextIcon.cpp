#include "ui/TextIcon.h"

#include <shellscalingapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>

#include "ui/Graphics.h"

using Microsoft::WRL::ComPtr;

namespace llaime {

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

HICON CreateTaskbarTextIcon(std::wstring_view text, int size) {
  // Render the glyph's coverage with Direct2D into a WIC bitmap, then build a straight-alpha
  // icon whose color is the taskbar text color and whose alpha is that coverage.
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
  ComPtr<IDWriteTextFormat> format;
  DWriteFactory()->CreateTextFormat(L"Microsoft JhengHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    size * 0.8f, L"zh-TW", &format);
  if (!format) return nullptr;
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  ComPtr<ID2D1SolidColorBrush> brush;
  target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
  target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  target->BeginDraw();
  target->Clear(D2D1::ColorF(0, 0, 0, 0));
  const std::wstring str(text);
  target->DrawText(str.c_str(), static_cast<UINT32>(str.size()), format.Get(),
                   D2D1::RectF(0, 0, static_cast<float>(size), static_cast<float>(size)),
                   brush.Get());
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
