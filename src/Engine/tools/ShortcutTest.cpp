// End-to-end test of user phrase shortcuts through the installed IME: with 「」 on "mk" and
// 『』 on "mkk" in a temporary user.db, types ` p m k (and variations) in our own Edit control
// and checks what was committed.
//
//   LLaIME.ShortcutTest
//
// The test stops the running LLaIME.Engine and starts its own without a ranker on the
// temporary user.db; the user's keyboard is restored afterwards. Keystrokes are sent with
// SendInput, only while the test window is in front.
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <msctf.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "TestDesktop.h"
#include "UserStore.h"
#include "llaime/UserDictionary.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using namespace llaime;
using namespace llaime::testing;

namespace {

bool Type(HWND target, const std::vector<WORD>& keys) {
  for (WORD vk : keys) {
    if (GetForegroundWindow() != target) return false;
    Key(vk);
  }
  Pump(600);
  return GetForegroundWindow() == target;
}

}  // namespace

int wmain() {
  _setmode(_fileno(stdout), _O_U8TEXT);
  _setmode(_fileno(stderr), _O_U8TEXT);
  const fs::path engineExe = InstallRoot() / L"engine" / L"LLaIME.Engine.exe";
  if (!fs::exists(engineExe)) {
    fwprintf(stderr, L"LLaIME is not installed (scripts/dev-install.ps1)\n");
    return 1;
  }

  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const fs::path db = fs::path(tmp) / L"llaime-shortcuttest.db";
  auto removeDb = [&] {
    for (const wchar_t* suffix : {L"", L"-wal", L"-shm"}) {
      std::error_code ec;
      fs::remove(db.wstring() + suffix, ec);
    }
  };
  removeDb();
  {
    UserStore store;
    if (!store.Open(db.wstring()) || !store.AddPhrase({}, L"「」", 0, L"mk") ||
        !store.AddPhrase({}, L"『』", 0, L"mkk")) {
      fwprintf(stderr, L"cannot prepare %ls\n", db.c_str());
      return 1;
    }
  }

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
  Sleep(500);

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

  HWND window = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"LLaIME shortcut test",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 420, 120, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
  HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 10, 10, 380, 28, window,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
  SetForegroundWindow(window);
  SetFocus(edit);
  // The TIP fetches the user dictionary (with the shortcuts) when it starts.
  Pump(2500);

  const WORD grave = VK_OEM_3;
  struct Case {
    const wchar_t* name;
    std::vector<WORD> keys;
    std::wstring expected;
    std::wstring text;
    bool typed = false;
  };
  std::vector<Case> cases = {
      // mk is also the start of mkk: Enter takes mk.
      {L"` p m k Enter", {grave, 'P', 'M', 'K', VK_RETURN}, L"「」"},
      // mkk is no other shortcut's start: it commits by itself.
      {L"` p m k k", {grave, 'P', 'M', 'K', 'K'}, L"『』"},
      // In the middle of a composition: 我 (ㄨㄛˇ), then the shortcut, then Enter.
      {L"ㄨㄛˇ ` p m k k Enter", {'J', 'I', '3', grave, 'P', 'M', 'K', 'K', VK_RETURN}, L"我『』"},
      // A key no shortcut has beeps and is ignored; Esc leaves shortcut mode and drops the `.
      {L"` p z m k k", {grave, 'P', 'Z', 'M', 'K', 'K'}, L"『』"},
      {L"` p m Esc", {grave, 'P', 'M', VK_ESCAPE}, L""},
      // Backspace out of shortcut mode leaves the ` itself.
      {L"` p Back Enter", {grave, 'P', VK_BACK, VK_RETURN}, L"`"},
  };
  for (Case& c : cases) {
    if (!switched) break;
    SetWindowTextW(edit, L"");
    SetForegroundWindow(window);
    SetFocus(edit);
    Pump(400);
    c.typed = Type(window, c.keys);
    wchar_t text[64] = {};
    GetWindowTextW(edit, text, 64);
    c.text = text;
    Key(VK_ESCAPE);  // nothing left composing for the next case
    Pump(200);
    if (!c.typed) break;
  }
  DestroyWindow(window);

  if (switched && previous.dwProfileType) {
    profiles->ActivateProfile(previous.dwProfileType, previous.langid, previous.clsid,
                              previous.guidProfile, previous.hkl,
                              TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
  }
  TerminateProcess(engine.hProcess, 0);
  WaitForSingleObject(engine.hProcess, 5000);
  CloseHandle(engine.hProcess);
  CloseHandle(engine.hThread);
  removeDb();
  profiles.Reset();  // before CoUninitialize
  CoUninitialize();

  if (!switched) {
    fwprintf(stderr, L"cannot switch to LLaIME 注音\n");
    return 1;
  }
  int failures = 0;
  for (const Case& c : cases) {
    const bool pass = c.typed && c.text == c.expected;
    wprintf(L"%-24ls %-6ls %ls（預期 %ls）\n", c.name, pass ? L"pass" : L"FAIL",
            c.typed ? (c.text.empty() ? L"（空白）" : c.text.c_str())
                    : L"(the test window lost the focus; stopped typing)",
            c.expected.empty() ? L"（空白）" : c.expected.c_str());
    failures += !pass;
  }
  return failures == 0 ? 0 : 1;
}
