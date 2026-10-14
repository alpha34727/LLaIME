#include "ui/Theme.h"

#include <windows.h>

namespace llaime {
namespace {

D2D1_COLOR_F Rgb(UINT32 rgb, float alpha = 1.0f) {
  return D2D1::ColorF(rgb, alpha);
}

bool AppsUseLightTheme() {
  DWORD value = 1, size = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  return value != 0;
}

// Explorer's AccentPalette holds 8 RGBA colors: Light3, Light2, Light1, Base, Dark1, Dark2,
// Dark3, (unused). Windows uses Dark1 for indicators on light surfaces and Light2 on dark ones.
bool ReadAccent(int index, D2D1_COLOR_F* out) {
  BYTE palette[32];
  DWORD size = sizeof(palette);
  if (RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                   L"AccentPalette", RRF_RT_REG_BINARY, nullptr, palette, &size) != ERROR_SUCCESS ||
      size < 32) {
    return false;
  }
  const BYTE* c = palette + index * 4;
  *out = D2D1::ColorF(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1.0f);
  return true;
}

}  // namespace

Theme Theme::Current() {
  Theme t;
  t.dark = !AppsUseLightTheme();
  if (t.dark) {
    t.background = Rgb(0x2C2C2C);
    t.border = Rgb(0x000000, 0.25f);
    t.text = Rgb(0xFFFFFF);
    t.secondaryText = Rgb(0xFFFFFF, 0.6f);
    t.hoverFill = Rgb(0xFFFFFF, 0.06f);
    t.selectedFill = Rgb(0xFFFFFF, 0.08f);
    t.separator = Rgb(0xFFFFFF, 0.08f);
    t.pageDot = Rgb(0xFFFFFF, 0.36f);
    t.pageDotActive = Rgb(0xFFFFFF, 0.72f);
    if (!ReadAccent(1, &t.accent)) t.accent = Rgb(0x60CDFF);
  } else {
    t.background = Rgb(0xF9F9F9);
    t.border = Rgb(0x000000, 0.06f);
    t.text = Rgb(0x1A1A1A);
    t.secondaryText = Rgb(0x000000, 0.55f);
    t.hoverFill = Rgb(0x000000, 0.04f);
    t.selectedFill = Rgb(0x000000, 0.06f);
    t.separator = Rgb(0x000000, 0.08f);
    t.pageDot = Rgb(0x000000, 0.30f);
    t.pageDotActive = Rgb(0x000000, 0.62f);
    if (!ReadAccent(4, &t.accent)) t.accent = Rgb(0x005FB8);
  }
  return t;
}

}  // namespace llaime
