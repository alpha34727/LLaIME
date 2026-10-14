#pragma once
#include <windows.h>

namespace llaime {

HRESULT RegisterComServer();
void UnregisterComServer();
HRESULT RegisterProfiles();
void UnregisterProfiles();
HRESULT RegisterCategories();
void UnregisterCategories();

}  // namespace llaime
