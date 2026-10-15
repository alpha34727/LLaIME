#include "llaime/ImeSettings.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace llaime {
namespace {

// S-1-15-2-1: ALL APPLICATION PACKAGES.
bool GrantRead(const std::wstring& object, SE_OBJECT_TYPE type, DWORD rights) {
  PSID appContainers = nullptr;
  if (!ConvertStringSidToSidW(L"S-1-15-2-1", &appContainers)) return false;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  bool ok = false;
  std::wstring name = object;
  if (GetNamedSecurityInfoW(name.data(), type, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                            &dacl, nullptr, &sd) == ERROR_SUCCESS) {
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = rights;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(appContainers);
    PACL updated = nullptr;
    if (SetEntriesInAclW(1, &access, dacl, &updated) == ERROR_SUCCESS) {
      ok = SetNamedSecurityInfoW(name.data(), type, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                 updated, nullptr) == ERROR_SUCCESS;
      LocalFree(updated);
    }
    LocalFree(sd);
  }
  LocalFree(appContainers);
  return ok;
}

std::wstring ReadString(HKEY key, const wchar_t* name, const std::wstring& fallback) {
  wchar_t buf[MAX_PATH * 2];
  DWORD bytes = sizeof(buf);
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buf, &bytes) != ERROR_SUCCESS) {
    return fallback;
  }
  return buf;
}

}  // namespace

ImeSettings LoadImeSettings() {
  ImeSettings settings;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return settings;
  }
  DWORD bytes = 0;
  if (RegGetValueW(key, nullptr, L"DisabledLexicons", RRF_RT_REG_MULTI_SZ, nullptr, nullptr,
                   &bytes) == ERROR_SUCCESS &&
      bytes > 0) {
    std::wstring buf(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, L"DisabledLexicons", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(),
                     &bytes) == ERROR_SUCCESS) {
      for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1) {
        settings.disabledLexicons.emplace_back(p);
      }
    }
  }
  settings.userLexiconDir = ReadString(key, L"UserLexiconDir", L"");
  settings.layout = ReadString(key, L"Layout", L"standard");
  DWORD generation = 0;
  bytes = sizeof(generation);
  RegGetValueW(key, nullptr, L"Generation", RRF_RT_REG_DWORD, nullptr, &generation, &bytes);
  settings.generation = generation;
  RegCloseKey(key);
  return settings;
}

uint32_t ReadSettingsGeneration() {
  DWORD generation = 0;
  DWORD bytes = sizeof(generation);
  RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Generation", RRF_RT_REG_DWORD, nullptr,
               &generation, &bytes);
  return generation;
}

bool SaveImeSettings(const ImeSettings& settings) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_READ | KEY_WRITE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  GrantRead(L"CURRENT_USER\\Software\\LLaIME", SE_REGISTRY_KEY, KEY_READ);
  std::wstring multi;
  for (const auto& id : settings.disabledLexicons) multi += id + L'\0';
  multi += L'\0';
  auto setString = [&](const wchar_t* name, const std::wstring& value, DWORD type) {
    const DWORD bytes = static_cast<DWORD>((value.size() + (type == REG_SZ ? 1 : 0)) *
                                           sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(value.c_str()),
                          bytes) == ERROR_SUCCESS;
  };
  bool ok = setString(L"DisabledLexicons", multi, REG_MULTI_SZ) &&
            setString(L"UserLexiconDir", settings.userLexiconDir, REG_SZ) &&
            setString(L"Layout", settings.layout, REG_SZ);
  // A new value every time, whatever the previous one was (another process may have saved).
  DWORD generation = GetTickCount() | 1;
  if (generation == ReadSettingsGeneration()) generation += 2;
  ok = ok && RegSetValueExW(key, L"Generation", 0, REG_DWORD,
                            reinterpret_cast<const BYTE*>(&generation),
                            sizeof(generation)) == ERROR_SUCCESS;
  RegCloseKey(key);
  return ok;
}

bool GrantAppContainerRead(const std::wstring& folder) {
  return GrantRead(folder, SE_FILE_OBJECT, GENERIC_READ | GENERIC_EXECUTE);
}

std::wstring UserDataDir() {
  PWSTR local = nullptr;
  std::wstring path;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
    path = std::wstring(local) + L"\\LLaIME";
    CoTaskMemFree(local);
  }
  return path;
}

}  // namespace llaime
