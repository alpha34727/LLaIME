#include "KeyMapping.h"

namespace llaime {

wchar_t UsCharFromVirtualKey(WPARAM vk, bool shift) {
  if (vk >= 'A' && vk <= 'Z') return static_cast<wchar_t>(shift ? vk : vk - 'A' + 'a');
  if (vk >= '0' && vk <= '9') {
    static constexpr wchar_t kShiftedDigits[] = L")!@#$%^&*(";
    return shift ? kShiftedDigits[vk - '0'] : static_cast<wchar_t>(vk);
  }
  struct Oem {
    WPARAM vk;
    wchar_t normal, shifted;
  };
  static constexpr Oem kOem[] = {
      {VK_SPACE, L' ', L' '},       {VK_OEM_1, L';', L':'},      {VK_OEM_PLUS, L'=', L'+'},
      {VK_OEM_COMMA, L',', L'<'},   {VK_OEM_MINUS, L'-', L'_'},  {VK_OEM_PERIOD, L'.', L'>'},
      {VK_OEM_2, L'/', L'?'},       {VK_OEM_3, L'`', L'~'},      {VK_OEM_4, L'[', L'{'},
      {VK_OEM_5, L'\\', L'|'},      {VK_OEM_6, L']', L'}'},      {VK_OEM_7, L'\'', L'"'},
  };
  for (const Oem& o : kOem) {
    if (o.vk == vk) return shift ? o.shifted : o.normal;
  }
  return 0;
}

Modifiers Modifiers::Current() {
  Modifiers m;
  m.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  m.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  m.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
  return m;
}

}  // namespace llaime
