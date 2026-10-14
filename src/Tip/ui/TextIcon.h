#pragma once
#include <windows.h>

#include <string_view>

namespace llaime {

// Creates a monochrome (mask-only) icon showing `text`. Combined with
// TF_LBI_STYLE_TEXTCOLORICON, Windows recolors its black pixels to the taskbar's text color,
// so the icon follows the light/dark taskbar theme. Caller owns the icon (DestroyIcon).
HICON CreateMonochromeTextIcon(std::wstring_view text, int size);

// Icon edge length for language bar items at the current system DPI.
int LangBarIconSize();

}  // namespace llaime
