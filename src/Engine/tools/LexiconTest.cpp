// End-to-end test of turning a lexicon layer off, through the installed IME: types the reading
// of 總狀花序 (a term only the Ministry of Education dictionary layer has) in our own Edit
// control, with that layer on and then off, and checks what was committed.
//
//   LLaIME.LexiconTest
//
// The layer is turned off the way the settings app does it (HKCU\Software\LLaIME, new
// generation), while the IME is loaded: the TIP must switch layers before its next
// composition, without restarting the app. The test stops the running LLaIME.Engine and
// starts its own without a ranker on a temporary user.db, so the result is the lexicon's own
// conversion; the user's settings and keyboard are restored afterwards. Keystrokes are sent
// with SendInput, only while the test window is in front.
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "TestDesktop.h"
#include "llaime/ImeSettings.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using namespace llaime;
using namespace llaime::testing;

namespace {

constexpr wchar_t kWord[] = L"總狀花序";
constexpr wchar_t kLayer[] = L"moe";

// ㄗㄨㄥˇ ㄓㄨㄤˋ ㄏㄨㄚ ㄒㄩˋ on the standard layout, then Enter.
bool TypeWord(HWND target) {
  const WORD keys[] = {'Y', 'J', VK_OEM_2, '3', '5', 'J', VK_OEM_1, '4',
                       'C', 'J', '8', VK_SPACE, 'V', 'M', '4'};
  for (WORD vk : keys) {
    if (GetForegroundWindow() != target) return false;
    Key(vk);
  }
  Pump(500);
  if (GetForegroundWindow() != target) return false;
  Key(VK_RETURN);
  Pump(500);
  return true;
}

}  // namespace

int wmain() {
  // Wide output as UTF-8 (the CRT would turn Chinese into ?).
  _setmode(_fileno(stdout), _O_U8TEXT);
  _setmode(_fileno(stderr), _O_U8TEXT);
  SetConsoleOutputCP(CP_UTF8);
  const fs::path root = InstallRoot();
  const fs::path engineExe = root / L"engine" / L"LLaIME.Engine.exe";
  if (root.empty() || !fs::exists(root / L"data" / L"lexicons" / (std::wstring(kLayer) + L".bin"))) {
    fwprintf(stderr, L"LLaIME is not installed with lexicon layers (scripts/dev-install.ps1)\n");
    return 1;
  }

  HKEY existing = nullptr;
  const bool hadKey =
      RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &existing) == ERROR_SUCCESS;
  if (existing) RegCloseKey(existing);
  const ImeSettings original = LoadImeSettings();

  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const fs::path db = fs::path(tmp) / L"llaime-lexicontest.db";
  StopEngines();
  std::wstring cmd = L"\"" + engineExe.wstring() + L"\" --userdb \"" + db.wstring() +
                     L"\" --ranker llm --model none --laya none";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION engine{};
  if (!CreateProcessW(engineExe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, engineExe.parent_path().c_str(), &si, &engine)) {
    fwprintf(stderr, L"cannot start %ls\n", engineExe.c_str());
    return 1;
  }

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ComPtr<ITfInputProcessorProfileMgr> profiles;
  TF_INPUTPROCESSORPROFILE previous{};
  bool switched = false;
  if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&profiles)))) {
    profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &previous);
    switched = SUCCEEDED(profiles->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, kLangIdZhTw,
                                                   kClsidTextService, kProfileBopomofo, nullptr,
                                                   TF_IPPMF_FORSESSION |
                                                       TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE));
  }

  HWND window = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"LLaIME lexicon test",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 420, 120, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
  HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 10, 380, 28, window,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
  Pump(500);

  struct Run {
    const wchar_t* name;
    bool layerOn;
    std::wstring text;
    bool typed = false;
  };
  std::vector<Run> runs = {{L"教育部詞庫啟用", true}, {L"教育部詞庫停用", false},
                           {L"再次啟用", true}};
  for (Run& run : runs) {
    if (!switched) break;
    ImeSettings settings = original;
    std::erase(settings.disabledLexicons, std::wstring(kLayer));
    if (!run.layerOn) settings.disabledLexicons.push_back(kLayer);
    SaveImeSettings(settings);
    Pump(1500);  // the TIP checks the settings at most once a second
    SetWindowTextW(edit, L"");
    SetForegroundWindow(window);
    SetFocus(edit);
    Pump(500);
    run.typed = TypeWord(window);
    wchar_t text[64] = {};
    GetWindowTextW(edit, text, 64);
    run.text = text;
    if (!run.typed) break;
  }
  DestroyWindow(window);

  // Restore the user's settings, keyboard and engine (the TIPs restart it on demand).
  if (hadKey) {
    SaveImeSettings(original);
  } else {
    RegDeleteTreeW(HKEY_CURRENT_USER, kSettingsKey);
  }
  if (switched && previous.dwProfileType) {
    profiles->ActivateProfile(previous.dwProfileType, previous.langid, previous.clsid,
                              previous.guidProfile, previous.hkl,
                              TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
  }
  TerminateProcess(engine.hProcess, 0);
  WaitForSingleObject(engine.hProcess, 5000);
  CloseHandle(engine.hProcess);
  CloseHandle(engine.hThread);
  for (const wchar_t* suffix : {L"", L"-wal", L"-shm"}) {
    std::error_code ec;
    fs::remove(db.wstring() + suffix, ec);
  }
  profiles.Reset();  // before CoUninitialize
  CoUninitialize();

  if (!switched) {
    fwprintf(stderr, L"cannot switch to LLaIME 注音\n");
    return 1;
  }
  int failures = 0;
  for (const Run& run : runs) {
    const bool pass = run.typed && !run.text.empty() && (run.text == kWord) == run.layerOn;
    wprintf(L"%-12ls %-6ls %ls\n", run.name, pass ? L"pass" : L"FAIL",
            run.typed ? run.text.c_str() : L"(the test window lost the focus; stopped typing)");
    failures += !pass;
  }
  return failures == 0 ? 0 : 1;
}
