// Stress test for host apps: types Bopomofo into another program's window with LLaIME active,
// saving (Ctrl+S) while a composition is open, the way an autosave lands mid-typing, and checks
// that the program is still alive after every round.
//
//   LLaIME.TypeTest --title <part of the window title> [--pid <process id>] [--rounds 10]
//                   [--no-save] [--punct]
//
// --punct types long runs of punctuation (Shift and Ctrl keys, keys outside the layout) mixed
// with syllables instead of three words.
//
// Keys go only to the window of that process whose title contains --title (Word keeps every
// open document in one process, so the process alone does not say which window), and only
// while that window is in front; typing stops if anything else comes forward. The keyboard is switched to LLaIME 注音 for the session and
// back afterwards. Meant for a throwaway document (e.g. a new Word document saved to %TEMP%).
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>

#include "TestDesktop.h"

using Microsoft::WRL::ComPtr;
using namespace llaime::testing;

namespace {

struct Target {
  DWORD pid;
  std::wstring title;
  HWND hwnd = nullptr;
};

BOOL CALLBACK FindWindowOf(HWND hwnd, LPARAM lp) {
  auto* t = reinterpret_cast<Target*>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  wchar_t title[512] = {};
  GetWindowTextW(hwnd, title, 512);
  if ((t->pid == 0 || pid == t->pid) && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER) &&
      !t->title.empty() && wcsstr(title, t->title.c_str())) {
    t->hwnd = hwnd;
    t->pid = pid;
    return FALSE;
  }
  return TRUE;
}

bool Alive(HANDLE process) { return WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }

HWND g_target = nullptr;

// That very window, not just any window of the process.
bool InFront(DWORD) { return GetForegroundWindow() == g_target; }

struct Stroke {
  WORD vk;
  bool shift = false;
  bool ctrl = false;
};

void Press(const Stroke& k) {
  INPUT in[6] = {};
  UINT n = 0;
  auto add = [&](WORD vk, bool up) {
    in[n].type = INPUT_KEYBOARD;
    in[n].ki.wVk = vk;
    in[n].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    ++n;
  };
  if (k.ctrl) add(VK_CONTROL, false);
  if (k.shift) add(VK_SHIFT, false);
  add(k.vk, false);
  add(k.vk, true);
  if (k.shift) add(VK_SHIFT, true);
  if (k.ctrl) add(VK_CONTROL, true);
  SendInput(n, in, sizeof(INPUT));
  Sleep(60);
}

// ㄧˋㄧˋ ㄕˋㄧㄢˋ ㄉㄧㄢˋㄋㄠˇ: three words, left composing.
const Stroke kReading[] = {{'U'}, {'4'}, {'U'}, {'4'}, {'G'}, {'4'}, {'U'}, {'0'},
                           {'4'}, {'2'}, {'U'}, {'0'}, {'4'}, {'S'}, {'L'}, {'3'}};
// Punctuation the way people type it in Bopomofo: Ctrl+，。、；：？！, Shift+symbols, and the
// keys outside the layout ([ ] ' = `), between syllables.
const Stroke kPunct[] = {
    {VK_OEM_COMMA, false, true}, {VK_OEM_PERIOD, false, true}, {VK_OEM_4}, {VK_OEM_6},
    {'U'}, {'4'}, {VK_OEM_6}, {VK_OEM_COMMA, false, true}, {'1', true}, {'2', true},
    {'3', true}, {VK_OEM_2, true}, {VK_OEM_1, true}, {VK_OEM_7}, {VK_OEM_PLUS}, {VK_OEM_3},
    {'9', true}, {'0', true}, {'G'}, {'4'}, {VK_OEM_PERIOD, false, true}, {VK_OEM_4, true},
    {VK_OEM_6, true}, {VK_OEM_1, false, true}, {VK_OEM_2, true, true}, {'1', true, true},
    {VK_OEM_7, false, true}, {VK_OEM_5}, {VK_OEM_5, true}, {VK_OEM_MINUS, true}};

}  // namespace

int wmain(int argc, wchar_t** argv) {
  _setmode(_fileno(stdout), _O_U8TEXT);
  _setmode(_fileno(stderr), _O_U8TEXT);
  Target target{0};
  int rounds = 10;
  bool save = true;
  bool punct = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--pid" && i + 1 < argc) target.pid = _wtoi(argv[++i]);
    if (a == L"--rounds" && i + 1 < argc) rounds = _wtoi(argv[++i]);
    if (a == L"--title" && i + 1 < argc) target.title = argv[++i];
    if (a == L"--no-save") save = false;
    if (a == L"--punct") punct = true;
  }
  EnumWindows(FindWindowOf, reinterpret_cast<LPARAM>(&target));
  HANDLE process =
      target.hwnd ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid)
                  : nullptr;
  if (!process || !target.hwnd) {
    fwprintf(stderr, L"no window of process %lu with \"%ls\" in its title\n", target.pid,
             target.title.c_str());
    return 2;
  }
  g_target = target.hwnd;

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

  // Bring it to the front without keystrokes.
  if (GetForegroundWindow() != target.hwnd) {
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD self = GetCurrentThreadId();
    AttachThreadInput(self, fgThread, TRUE);
    SetForegroundWindow(target.hwnd);
    BringWindowToTop(target.hwnd);
    AttachThreadInput(self, fgThread, FALSE);
  }
  Pump(1500);
  int completed = 0;
  const wchar_t* stop = nullptr;
  for (int r = 0; r < rounds && !stop; ++r) {
    if (punct) {
      // Two runs, so the composition passes the 40-element commit point.
      for (int run = 0; run < 2 && !stop; ++run) {
        for (const Stroke& k : kPunct) {
          if (!InFront(target.pid)) { stop = L"the window lost the focus; stopped typing"; break; }
          Press(k);
        }
      }
    } else {
      for (const Stroke& k : kReading) {
        if (!InFront(target.pid)) { stop = L"the window lost the focus; stopped typing"; break; }
        Press(k);
      }
    }
    if (stop) break;
    Pump(300);  // the model's reranking arrives meanwhile
    if (save) {
      if (!InFront(target.pid)) { stop = L"the window lost the focus; stopped typing"; break; }
      Key('S', true);  // save with the composition open
      Pump(2500);
    }
    if (!Alive(process)) { stop = L"THE PROGRAM EXITED (crash)"; break; }
    if (!InFront(target.pid)) { stop = L"the window lost the focus; stopped typing"; break; }
    for (WORD vk : {'U', '4'}) Key(vk);  // keep typing after the save
    Pump(300);
    Key(VK_RETURN);
    Pump(500);
    if (!Alive(process)) { stop = L"THE PROGRAM EXITED (crash)"; break; }
    ++completed;
    wprintf(L"round %d ok\n", r + 1);
  }

  if (switched && previous.dwProfileType) {
    profiles->ActivateProfile(previous.dwProfileType, previous.langid, previous.clsid,
                              previous.guidProfile, previous.hkl,
                              TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
  }
  profiles.Reset();
  CoUninitialize();
  wprintf(L"%d/%d rounds%ls%ls\n", completed, rounds, stop ? L": " : L"", stop ? stop : L"");
  return completed == rounds ? 0 : 1;
}
