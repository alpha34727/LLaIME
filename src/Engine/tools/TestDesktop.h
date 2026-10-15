#pragma once
// Shared by the end-to-end tests that type through the installed IME (PrivacyTest,
// LexiconTest): the installed root, stopping the user's engine, keystrokes and message pumping.
#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>

namespace llaime::testing {

// src/Tip/Globals.h
inline constexpr CLSID kClsidTextService = {
    0x5b72f657, 0xf087, 0x4472, {0xa8, 0x27, 0x7f, 0xd8, 0xeb, 0x60, 0x8d, 0xe6}};
inline constexpr GUID kProfileBopomofo = {
    0x671f61eb, 0xe460, 0x42b0, {0x8b, 0x3a, 0xc9, 0x0c, 0xb1, 0x48, 0x87, 0x16}};
inline constexpr LANGID kLangIdZhTw = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);

// The installed root, from the TIP's COM registration: <root>\x64\LLaIME.Tip.dll.
inline std::filesystem::path InstallRoot() {
  wchar_t value[MAX_PATH] = {};
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CLASSES_ROOT,
                   L"CLSID\\{5B72F657-F087-4472-A827-7FD8EB608DE6}\\InprocServer32", nullptr,
                   RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
    return {};
  }
  return std::filesystem::path(value).parent_path().parent_path();
}

inline void StopEngines() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  PROCESSENTRY32W pe{sizeof(pe)};
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    if (_wcsicmp(pe.szExeFile, L"LLaIME.Engine.exe") != 0) continue;
    if (HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID)) {
      TerminateProcess(p, 0);
      WaitForSingleObject(p, 5000);
      CloseHandle(p);
    }
  }
  CloseHandle(snap);
}

inline void Key(WORD vk, bool ctrl = false) {
  INPUT inputs[4] = {};
  UINT n = 0;
  auto add = [&](WORD k, bool up) {
    INPUT& in = inputs[n++];
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = k;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (k == VK_DOWN || k == VK_UP || k == VK_LEFT || k == VK_RIGHT) {
      in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
  };
  if (ctrl) add(VK_CONTROL, false);
  add(vk, false);
  add(vk, true);
  if (ctrl) add(VK_CONTROL, true);
  SendInput(n, inputs, sizeof(INPUT));
  Sleep(120);
}

inline void Pump(DWORD ms) {
  const ULONGLONG end = GetTickCount64() + ms;
  while (GetTickCount64() < end) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(20);
  }
}

}  // namespace llaime::testing
