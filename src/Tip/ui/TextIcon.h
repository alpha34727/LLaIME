#pragma once
#include <windows.h>

#include <string_view>

namespace llaime {

// Creates an anti-aliased 32-bpp icon showing `text` in the taskbar's text color: white on a
// dark taskbar, black on a light one. (Windows 11 does not recolor TF_LBI_STYLE_TEXTCOLORICON
// icons, so the theme has to be applied here.) With `shield`, a small shield badge in the
// bottom-right corner marks privacy mode, like Microsoft Bopomofo. Caller owns the icon.
HICON CreateTaskbarTextIcon(std::wstring_view text, int size, bool shield = false);

// Icon edge length for language bar items at the current system DPI.
int LangBarIconSize();

// True when the Windows (taskbar) theme is light.
bool SystemUsesLightTheme();

}  // namespace llaime
