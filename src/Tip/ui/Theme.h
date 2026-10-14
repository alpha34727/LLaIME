#pragma once
#include <d2d1.h>

namespace llaime {

// Colors for IME UI, following the system light/dark setting and accent color.
struct Theme {
  bool dark = false;
  D2D1_COLOR_F background;
  D2D1_COLOR_F border;
  D2D1_COLOR_F text;
  D2D1_COLOR_F secondaryText;  // candidate numbers, disabled glyphs
  D2D1_COLOR_F hoverFill;
  D2D1_COLOR_F selectedFill;
  D2D1_COLOR_F accent;  // selection indicator pill
  D2D1_COLOR_F separator;
  D2D1_COLOR_F pageDot;
  D2D1_COLOR_F pageDotActive;

  static Theme Current();
};

}  // namespace llaime
