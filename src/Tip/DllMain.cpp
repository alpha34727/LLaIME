#include "ClassFactory.h"
#include "Globals.h"
#include "Register.h"
#include "EngineClient.h"
#include "ui/CandidateWindow.h"
#include "ui/NoticeWindow.h"
#include "ui/PopupMenu.h"
#include "ui/ThemeWatcher.h"

using namespace llaime;

namespace {
ClassFactory g_classFactory;
}

BOOL APIENTRY DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_hInst = hInst;
    DisableThreadLibraryCalls(hInst);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = nullptr;
  if (clsid != CLSID_TextService) return CLASS_E_CLASSNOTAVAILABLE;
  return g_classFactory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() {
  if (g_dllRefCount != 0) return S_FALSE;
  CandidateWindow::UnregisterClass();
  NoticeWindow::UnregisterClass();
  UnregisterPopupMenuClass();
  ThemeWatcher::UnregisterClass();
  EngineClient::UnregisterClass();
  return S_OK;
}

STDAPI DllUnregisterServer() {
  UnregisterCategories();
  UnregisterProfiles();
  UnregisterComServer();
  return S_OK;
}

STDAPI DllRegisterServer() {
  HRESULT hr = RegisterComServer();
  if (SUCCEEDED(hr)) hr = RegisterProfiles();
  if (SUCCEEDED(hr)) hr = RegisterCategories();
  if (FAILED(hr)) DllUnregisterServer();
  return hr;
}
