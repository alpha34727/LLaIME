#pragma once
#include <windows.h>

namespace llaime {

// Character a key produces on a US keyboard, independent of the active keyboard layout.
// Returns 0 for keys without a printable character.
wchar_t UsCharFromVirtualKey(WPARAM vk, bool shift);

struct Modifiers {
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
  static Modifiers Current();
};

}  // namespace llaime
