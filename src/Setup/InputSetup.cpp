// LLaIME.InputSetup.exe --add | --remove
//
// Adds LLaIME 注音 to the current user's keyboard list (Settings > Time & language > Language
// & region > 中文（台灣）> Keyboards), or removes it, with InstallLayoutOrTip from input.dll.
// The installer runs it as the installing user after registering the TIP, and before
// unregistering it on uninstall. Exit code 0 on success.
#include <windows.h>

namespace {

// zh-TW : CLSID of the text service : profile GUID (src/Tip/Globals.h).
constexpr wchar_t kTip[] =
    L"0404:{5B72F657-F087-4472-A827-7FD8EB608DE6}{671F61EB-E460-42B0-8B3A-C90CB1488716}";
constexpr DWORD kIlotUninstall = 0x00000001;

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
  const bool remove = wcsstr(cmdLine, L"--remove") != nullptr;
  if (!remove && !wcsstr(cmdLine, L"--add")) return 2;
  using Fn = BOOL(WINAPI*)(LPCWSTR, DWORD);
  HMODULE input = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  auto install = input ? reinterpret_cast<Fn>(GetProcAddress(input, "InstallLayoutOrTip")) : nullptr;
  if (!install) return 1;
  return install(kTip, remove ? kIlotUninstall : 0) ? 0 : 1;
}
