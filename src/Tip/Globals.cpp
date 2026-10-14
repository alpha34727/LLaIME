#include "Globals.h"

namespace llaime {

HINSTANCE g_hInst = nullptr;
LONG g_dllRefCount = 0;

void DllAddRef() { InterlockedIncrement(&g_dllRefCount); }
void DllRelease() { InterlockedDecrement(&g_dllRefCount); }

}  // namespace llaime
