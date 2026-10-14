#pragma once
#include <windows.h>

#include <string_view>

namespace llaime {

// Creates an anti-aliased 32-bpp icon showing `text` in the taskbar's text color: white on a
// dark taskbar, black on a light one. (Windows 11 does not recolor TF_LBI_STYLE_TEXTCOLORICON
// icons, so the theme has to be applied here.) Caller owns the icon (DestroyIcon).
HICON CreateTaskbarTextIcon(std::wstring_view text, int size);

// Icon edge length for language bar items at the current system DPI.
int LangBarIconSize();

// True when the Windows (taskbar) theme is light.
bool SystemUsesLightTheme();

}  // namespace llaime
