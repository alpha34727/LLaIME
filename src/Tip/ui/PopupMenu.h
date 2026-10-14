#pragma once
#include <windows.h>

#include <string>
#include <vector>

namespace llaime {

struct PopupMenuItem {
  UINT id = 0;
  std::wstring text;
  bool checked = false;
  bool separator = false;
  bool enabled = true;
};

// Shows a Windows 11 style context menu above the cursor (like the taskbar's own menus) and
// blocks until it closes. Colors follow the Windows (taskbar) theme. Returns the chosen item
// id, or 0 when dismissed by clicking elsewhere or pressing Esc.
UINT ShowPopupMenu(const std::vector<PopupMenuItem>& items);

// Called before the DLL unloads.
void UnregisterPopupMenuClass();

}  // namespace llaime
