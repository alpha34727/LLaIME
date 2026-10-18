#include "Guard.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>

namespace llaime {

void ReportException(const char* where, const char* what) noexcept {
  char line[1024];
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t exe[MAX_PATH] = L"?";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const wchar_t* name = wcsrchr(exe, L'\\');
  const int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "%04u-%02u-%02u %02u:%02u:%02u  %ls (pid %lu, thread %lu)  %s: %s\r\n",
                            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                            name ? name + 1 : exe, GetCurrentProcessId(), GetCurrentThreadId(),
                            where, what ? what : "");
  OutputDebugStringA(line);
  PWSTR local = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return;
  wchar_t path[MAX_PATH];
  _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\LLaIME", local);
  CreateDirectoryW(path, nullptr);
  _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\LLaIME\\tip-errors.log", local);
  CoTaskMemFree(local);
  HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  if (n > 0) WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
  CloseHandle(file);
}

}  // namespace llaime
