// End-to-end test of privacy mode through the installed IME: types a reading, picks the second
// candidate and commits, in fields that are and are not private, then checks what reached the
// engine and user.db.
//
//   LLaIME.PrivacyTest [--browsers] [--only "Firefox text"]
//
// The test stops the running LLaIME.Engine and starts its own on a temporary user.db
// (the real one is never touched); the TIPs reconnect to it. It switches the session's keyboard
// to LLaIME 注音 and back, and sends keystrokes with SendInput, so the desktop must be left alone
// while it runs. With --browsers it also opens tests/privacy/*.html in Chrome, Edge and Firefox,
// normal and private windows, and closes each window it opened with Ctrl+W (only after checking
// that window is in front).
//
// For each field: a private field must teach nothing, flag every rank request it sends as
// private and send no context; a normal field must teach the pick and send unflagged requests.
#include <windows.h>
#include <msctf.h>
#include <inputscope.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "UserStore.h"
#include "llaime/Protocol.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace proto = llaime::protocol;

namespace {

// src/Tip/Globals.h
constexpr CLSID kClsidTextService = {
    0x5b72f657, 0xf087, 0x4472, {0xa8, 0x27, 0x7f, 0xd8, 0xeb, 0x60, 0x8d, 0xe6}};
constexpr GUID kProfileBopomofo = {
    0x671f61eb, 0xe460, 0x42b0, {0x8b, 0x3a, 0xc9, 0x0c, 0xb1, 0x48, 0x87, 0x16}};
constexpr LANGID kLangIdZhTw = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
constexpr wchar_t kPageTitle[] = L"LLaIME privacy test";

// The installed root, from the TIP's COM registration: <root>\x64\LLaIME.Tip.dll.
fs::path InstallRoot() {
  wchar_t value[MAX_PATH] = {};
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CLASSES_ROOT,
                   L"CLSID\\{5B72F657-F087-4472-A827-7FD8EB608DE6}\\InprocServer32", nullptr,
                   RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
    return {};
  }
  return fs::path(value).parent_path().parent_path();
}

void StopEngines() {
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

bool Transact(const std::vector<uint8_t>& request, std::vector<uint8_t>* reply) {
  const std::wstring name = proto::PipeName(proto::CurrentUserSid());
  HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return false;
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
  reply->assign(1 << 20, 0);
  DWORD read = 0;
  const bool ok = TransactNamedPipe(pipe, const_cast<uint8_t*>(request.data()),
                                    static_cast<DWORD>(request.size()), reply->data(),
                                    static_cast<DWORD>(reply->size()), &read, nullptr);
  reply->resize(read);
  CloseHandle(pipe);
  return ok;
}

bool Status(proto::StatusResponse* status) {
  std::vector<uint8_t> reply;
  if (!Transact(proto::EncodeStatusRequest(), &reply)) return false;
  proto::Reader in(reply.data(), reply.size());
  return in.Header() == proto::MessageType::StatusResponse && proto::Decode(in, status);
}

void Key(WORD vk, bool ctrl = false) {
  std::vector<INPUT> inputs;
  auto add = [&](WORD k, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = k;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (k == VK_DOWN || k == VK_UP || k == VK_LEFT || k == VK_RIGHT) {
      in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    inputs.push_back(in);
  };
  if (ctrl) add(VK_CONTROL, false);
  add(vk, false);
  add(vk, true);
  if (ctrl) add(VK_CONTROL, true);
  SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  Sleep(120);
}

void Pump(DWORD ms) {
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

// ㄧˋㄧˋ, open the candidates, pick the second, commit. Every key goes only to `target`: if
// another window comes to the front, typing stops (false) so nothing lands in the user's apps.
bool TypeAndPick(HWND target) {
  struct Step {
    WORD vk;
    DWORD pause;
  };
  const Step steps[] = {{'U', 0},         {'4', 0},   {'U', 0},         {'4', 400},
                        {VK_DOWN, 400},   {'2', 200}, {VK_RETURN, 0}};
  for (const auto& [vk, pause] : steps) {
    if (GetForegroundWindow() != target) return false;
    Key(vk);
    if (pause) Pump(pause);
  }
  return true;
}

struct Outcome {
  uint32_t ranks = 0, privateRanks = 0, contextRanks = 0, learns = 0;
  uint32_t dbVersion = 0;  // changes whenever user.db is written
};

class Probe {
 public:
  // One connection for the whole run: opening user.db writes to it (schema, pruning), which
  // would compete with the engine's own writes.
  explicit Probe(const fs::path& db) { store_.Open(db.wstring()); }
  Outcome Now() {
    Outcome o;
    proto::StatusResponse s;
    if (Status(&s)) {
      o.ranks = s.rankRequests;
      o.privateRanks = s.privateRankRequests;
      o.contextRanks = s.contextRankRequests;
      o.learns = s.learnRequests;
    }
    o.dbVersion = store_.Version();
    return o;
  }

 private:
  llaime::UserStore store_;
};

// What typing in a field led to: waits for a commit's learning to reach user.db (a normal
// field), or long enough that it would have (a private one).
Outcome Settle(Probe& probe, const Outcome& before) {
  Outcome now;
  for (int i = 0; i < 25; ++i) {
    Pump(200);
    now = probe.Now();
    if (now.learns > before.learns && now.dbVersion != before.dbVersion) break;
  }
  return now;
}

struct Result {
  std::wstring name;
  bool expectPrivate;
  bool pass;
  std::wstring detail;
};

Result Judge(const std::wstring& name, bool expectPrivate, const Outcome& a, const Outcome& b,
             const std::wstring& text) {
  const uint32_t ranks = b.ranks - a.ranks, priv = b.privateRanks - a.privateRanks;
  const uint32_t context = b.contextRanks - a.contextRanks;
  const uint32_t learns = b.learns - a.learns;
  const bool written = b.dbVersion != a.dbVersion;
  bool pass;
  if (expectPrivate) {
    pass = learns == 0 && !written && priv == ranks && context == 0;
  } else {
    pass = learns > 0 && written && priv == 0 && ranks > 0;
  }
  wchar_t detail[256];
  swprintf_s(detail, L"rank %u (private %u, context %u), learn %u, user.db %ls%ls%ls", ranks,
             priv, context, learns, written ? L"written" : L"unchanged",
             text.empty() ? L"" : L", text: ", text.c_str());
  return {name, expectPrivate, pass, detail};
}

// --- Win32 fields in our own window ---------------------------------------------------------

// msctf.dll exports SetInputScope, but the SDK has no import library for it.
void SetScope(HWND hwnd, InputScope scope) {
  using Fn = HRESULT(WINAPI*)(HWND, InputScope);
  static auto fn = reinterpret_cast<Fn>(
      GetProcAddress(LoadLibraryW(L"msctf.dll"), "SetInputScope"));
  if (fn) fn(hwnd, scope);
}

struct Field {
  const wchar_t* name;
  bool expectPrivate;
  DWORD style;
  InputScope scope;  // IS_DEFAULT: none set
};

std::vector<Result> RunWin32(Probe& probe) {
  const Field fields[] = {
      {L"Win32 Edit", false, 0, IS_DEFAULT},
      {L"Win32 Edit ES_PASSWORD", true, ES_PASSWORD, IS_DEFAULT},
      {L"Edit with input scope IS_PASSWORD", true, 0, IS_PASSWORD},
      {L"Edit with input scope IS_PRIVATE", true, 0, IS_PRIVATE},
  };
  HWND window = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", kPageTitle,
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 200, 420, 260, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
  std::vector<HWND> edits;
  int y = 10;
  for (const Field& f : fields) {
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | f.style, 10, y, 380, 28,
                                window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (f.scope != IS_DEFAULT) SetScope(edit, f.scope);
    edits.push_back(edit);
    y += 40;
  }
  Pump(500);
  std::vector<Result> results;
  for (size_t i = 0; i < edits.size(); ++i) {
    SetForegroundWindow(window);
    SetFocus(edits[i]);
    Pump(500);
    const Outcome before = probe.Now();
    if (!TypeAndPick(window)) {
      results.push_back({fields[i].name, fields[i].expectPrivate, false,
                         L"the test window lost the focus; stopped typing"});
      continue;
    }
    const Outcome after = Settle(probe, before);
    wchar_t text[64] = {};
    GetWindowTextW(edits[i], text, 64);
    results.push_back(Judge(fields[i].name, fields[i].expectPrivate, before, after,
                            fields[i].style & ES_PASSWORD ? L"" : text));
  }
  DestroyWindow(window);
  return results;
}

// --- Browsers ---------------------------------------------------------------------------------

HWND FindTestWindow() {
  struct Search {
    HWND found = nullptr;
  } search;
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 256);
        if (IsWindowVisible(hwnd) && wcsstr(title, kPageTitle)) {
          reinterpret_cast<Search*>(lp)->found = hwnd;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.found;
}

std::optional<Result> RunBrowser(Probe& probe, const std::wstring& name, const fs::path& exe,
                                 const std::wstring& args, bool expectPrivate) {
  if (!fs::exists(exe)) return std::nullopt;
  if (FindTestWindow()) {
    return Result{name, expectPrivate, false, L"a test window is already open"};
  }
  std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                      &pi)) {
    return Result{name, expectPrivate, false, L"cannot start the browser"};
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  HWND hwnd = nullptr;
  for (int i = 0; i < 100 && !hwnd; ++i) {
    Pump(150);
    hwnd = FindTestWindow();
  }
  if (!hwnd) return Result{name, expectPrivate, false, L"the test page did not open"};
  // Bring it to the front without keystrokes (a lone Alt would focus the browser's menu).
  if (GetForegroundWindow() != hwnd) {
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD self = GetCurrentThreadId();
    AttachThreadInput(self, fgThread, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    AttachThreadInput(self, fgThread, FALSE);
  }
  Pump(3500);  // page load and autofocus (a browser's first window can be slow)
  if (GetForegroundWindow() != hwnd) {
    return Result{name, expectPrivate, false, L"could not bring the test window to the front"};
  }
  const Outcome before = probe.Now();
  if (!TypeAndPick(hwnd)) {
    return Result{name, expectPrivate, false, L"the test window lost the focus; stopped typing"};
  }
  Result r = Judge(name, expectPrivate, before, Settle(probe, before), L"");
  if (GetForegroundWindow() == hwnd) {
    Key(VK_ESCAPE);  // drop anything still being composed
    if (GetForegroundWindow() == hwnd) Key('W', true);  // close the tab (and window) we opened
  }
  Pump(3000);  // let the focus settle on whatever window comes forward
  return r;
}

std::vector<Result> RunBrowsers(Probe& probe, const fs::path& pages, const std::wstring& only) {
  auto url = [&](const wchar_t* page) {
    std::wstring u = L"file:///" + (pages / page).generic_wstring();
    return u;
  };
  const fs::path chrome = L"C:/Program Files/Google/Chrome/Application/chrome.exe";
  const fs::path edge = L"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe";
  const fs::path firefox = L"C:/Program Files/Mozilla Firefox/firefox.exe";
  struct Case {
    const wchar_t* name;
    fs::path exe;
    std::wstring args;
    bool expectPrivate;
  };
  const std::vector<Case> cases = {
      {L"Chrome text", chrome, L"--new-window " + url(L"text.html"), false},
      {L"Chrome password", chrome, L"--new-window " + url(L"password.html"), true},
      {L"Chrome incognito text", chrome, L"--incognito " + url(L"text.html"), true},
      {L"Edge text", edge, L"--new-window " + url(L"text.html"), false},
      {L"Edge password", edge, L"--new-window " + url(L"password.html"), true},
      {L"Edge InPrivate text", edge, L"--inprivate " + url(L"text.html"), true},
      {L"Firefox text", firefox, L"-new-window " + url(L"text.html"), false},
      {L"Firefox password", firefox, L"-new-window " + url(L"password.html"), true},
      {L"Firefox private text", firefox, L"-private-window " + url(L"text.html"), true},
  };
  std::vector<Result> results;
  for (const Case& c : cases) {
    if (!only.empty() && !wcsstr(c.name, only.c_str())) continue;
    if (auto r = RunBrowser(probe, c.name, c.exe, c.args, c.expectPrivate)) {
      results.push_back(*r);
      wprintf(L"  %-28ls %ls\n", r->name.c_str(), r->pass ? L"pass" : L"FAIL");
    }
  }
  return results;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  bool browsers = false, win32 = true;
  std::wstring only;  // --only <text>: just the browser cases whose name contains it
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    browsers |= arg == L"--browsers";
    if (arg == L"--only" && i + 1 < argc) {
      only = argv[++i];
      browsers = true;
      win32 = false;
    }
  }

  wchar_t exePath[MAX_PATH];
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  // The engine built next to this test, else the installed one; the installed TIPs talk to
  // either (same protocol version).
  const fs::path root = InstallRoot();
  fs::path engineExe = fs::path(exePath).parent_path() / L"LLaIME.Engine.exe";
  if (!fs::exists(engineExe)) engineExe = root / L"engine" / L"LLaIME.Engine.exe";
  if (root.empty() || !fs::exists(engineExe)) {
    fwprintf(stderr, L"LLaIME is not installed (run scripts/dev-install.ps1)\n");
    return 1;
  }
  // build/x64/src/Engine/<config>/ -> repo
  const fs::path pages =
      fs::path(exePath).parent_path().parent_path().parent_path().parent_path().parent_path()
          .parent_path() / L"tests" / L"privacy";

  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const fs::path db = fs::path(tmp) / L"llaime-privacytest.db";
  auto removeDb = [&] {
    for (const wchar_t* suffix : {L"", L"-wal", L"-shm"}) {
      std::error_code ec;
      fs::remove(db.wstring() + suffix, ec);
    }
  };
  removeDb();

  // A test engine on a temporary user.db, with no ranker (conversion from the lexicon only).
  StopEngines();
  std::wstring cmd = L"\"" + engineExe.wstring() + L"\" --userdb \"" + db.wstring() +
                     L"\" --ranker llm --model none --laya none";
  // The engine's messages (never typed text) go to a log next to the temporary user.db.
  const fs::path log = fs::path(tmp) / L"llaime-privacytest.log";
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE logFile = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdError = si.hStdOutput = logFile;
  PROCESS_INFORMATION engine{};
  const BOOL started =
      CreateProcessW(engineExe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                     nullptr, engineExe.parent_path().c_str(), &si, &engine);
  CloseHandle(logFile);
  if (!started) {
    fwprintf(stderr, L"cannot start %ls\n", engineExe.c_str());
    return 1;
  }
  proto::StatusResponse status;
  for (int i = 0; i < 50 && !Status(&status); ++i) Sleep(100);
  if (WaitForSingleObject(engine.hProcess, 0) == WAIT_OBJECT_0) {
    fwprintf(stderr, L"another engine started first; close apps typing with LLaIME and retry\n");
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
  if (!switched) {
    fwprintf(stderr, L"cannot switch to LLaIME 注音 (is it added under 語言選項 → 鍵盤?)\n");
  }

  Probe probe(db);
  std::vector<Result> results;
  if (switched) {
    if (win32) {
      wprintf(L"Win32 fields...\n");
      results = RunWin32(probe);
    }
    if (browsers) {
      wprintf(L"Browsers...\n");
      auto more = RunBrowsers(probe, pages, only);
      results.insert(results.end(), more.begin(), more.end());
    }
  }

  // Restore the keyboard and the user's engine (the TIPs start it again on the next keystroke).
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

  int failures = switched ? 0 : 1;
  wprintf(L"\n%-36ls %-9ls %-6ls %ls\n", L"field", L"expected", L"result", L"detail");
  for (const Result& r : results) {
    wprintf(L"%-36ls %-9ls %-6ls %ls\n", r.name.c_str(), r.expectPrivate ? L"private" : L"normal",
            r.pass ? L"pass" : L"FAIL", r.detail.c_str());
    failures += !r.pass;
  }
  return failures == 0 ? 0 : 1;
}
