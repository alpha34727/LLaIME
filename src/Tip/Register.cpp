#include "Register.h"

#include <msctf.h>
#include <wrl/client.h>

#include <iterator>
#include <string>

#include "Globals.h"
#include "llaime/Version.h"
#include "resource.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

// Keyboard TIP that works in immersive/AppContainer apps, shows in the system tray,
// supports UIElement (apps may draw candidates themselves), and provides display
// attributes for the composition underline.
const GUID* const kCategories[] = {
    &GUID_TFCAT_TIP_KEYBOARD,
    &GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
    &GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,
    &GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
    &GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT,
    &GUID_TFCAT_TIPCAP_COMLESS,
    &GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER,
};

std::wstring ClsidString() {
  wchar_t buf[64];
  StringFromGUID2(CLSID_TextService, buf, static_cast<int>(std::size(buf)));
  return buf;
}

std::wstring ModulePath() {
  wchar_t path[MAX_PATH];
  DWORD len = GetModuleFileNameW(g_hInst, path, MAX_PATH);
  return std::wstring(path, len);
}

LSTATUS SetStringValue(HKEY key, const wchar_t* name, const std::wstring& value) {
  return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

HRESULT RegisterComServer() {
  const std::wstring keyPath = L"CLSID\\" + ClsidString();
  HKEY clsidKey = nullptr;
  LSTATUS st = RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath.c_str(), 0, nullptr, 0, KEY_WRITE,
                               nullptr, &clsidKey, nullptr);
  if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
  SetStringValue(clsidKey, nullptr, kProductName);

  HKEY inproc = nullptr;
  st = RegCreateKeyExW(clsidKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc,
                       nullptr);
  if (st == ERROR_SUCCESS) {
    SetStringValue(inproc, nullptr, ModulePath());
    SetStringValue(inproc, L"ThreadingModel", L"Apartment");
    RegCloseKey(inproc);
  }
  RegCloseKey(clsidKey);
  return HRESULT_FROM_WIN32(st);
}

void UnregisterComServer() {
  const std::wstring keyPath = L"CLSID\\" + ClsidString();
  RegDeleteTreeW(HKEY_CLASSES_ROOT, keyPath.c_str());
}

HRESULT RegisterProfiles() {
  ComPtr<ITfInputProcessorProfileMgr> mgr;
  HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&mgr));
  if (FAILED(hr)) return hr;

  const std::wstring iconFile = ModulePath();
  const std::wstring desc = kBopomofoProfileName;
  // A negative icon index references a resource ID rather than an ordinal.
  return mgr->RegisterProfile(CLSID_TextService, kLangIdZhTw, GUID_ProfileBopomofo, desc.c_str(),
                              static_cast<ULONG>(desc.size()), iconFile.c_str(),
                              static_cast<ULONG>(iconFile.size()),
                              static_cast<ULONG>(-IDI_BOPOMOFO), nullptr, 0, TRUE, 0);
}

void UnregisterProfiles() {
  ComPtr<ITfInputProcessorProfileMgr> mgr;
  if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&mgr)))) {
    mgr->UnregisterProfile(CLSID_TextService, kLangIdZhTw, GUID_ProfileBopomofo, 0);
  }
  // TSF keeps the text service's key (now empty of profiles and categories); remove it so an
  // uninstall leaves nothing behind.
  const std::wstring tipKey = L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + ClsidString();
  RegDeleteTreeW(HKEY_LOCAL_MACHINE, tipKey.c_str());
}

HRESULT RegisterCategories() {
  ComPtr<ITfCategoryMgr> cat;
  HRESULT hr =
      CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cat));
  if (FAILED(hr)) return hr;
  for (const GUID* g : kCategories) {
    hr = cat->RegisterCategory(CLSID_TextService, *g, CLSID_TextService);
    if (FAILED(hr)) return hr;
  }
  return S_OK;
}

void UnregisterCategories() {
  ComPtr<ITfCategoryMgr> cat;
  if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&cat)))) {
    for (const GUID* g : kCategories) {
      cat->UnregisterCategory(CLSID_TextService, *g, CLSID_TextService);
    }
  }
}

}  // namespace llaime
