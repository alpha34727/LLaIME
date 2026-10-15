#include "llaime/LexiconCatalog.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>

namespace llaime {
namespace {

namespace fs = std::filesystem;

std::wstring ReadIni(const std::wstring& ini, const wchar_t* key, const wchar_t* fallback = L"") {
  wchar_t buf[2048];
  GetPrivateProfileStringW(L"lexicon", key, fallback, buf, 2048, ini.c_str());
  return buf;
}

// Values are written on one line.
std::wstring OneLine(std::wstring s) {
  std::replace(s.begin(), s.end(), L'\r', L' ');
  std::replace(s.begin(), s.end(), L'\n', L' ');
  return s;
}

}  // namespace

void SortLexicons(std::vector<LexiconInfo>& lexicons) {
  std::stable_sort(lexicons.begin(), lexicons.end(),
                   [](const LexiconInfo& a, const LexiconInfo& b) {
                     if (a.required != b.required) return a.required;
                     if (a.builtIn != b.builtIn) return a.builtIn;
                     if (a.order != b.order) return a.order < b.order;
                     return a.name < b.name;
                   });
}

std::vector<LexiconInfo> ListLexicons(const std::wstring& dir, bool builtIn) {
  std::vector<LexiconInfo> out;
  std::error_code ec;
  for (const auto& item : fs::directory_iterator(dir, ec)) {
    if (!item.is_regular_file(ec) || item.path().extension() != L".bin") continue;
    LexiconInfo info;
    info.id = item.path().stem().wstring();
    info.path = item.path().wstring();
    info.builtIn = builtIn;
    const std::wstring ini = fs::path(item.path()).replace_extension(L".ini").wstring();
    info.name = ReadIni(ini, L"name", info.id.c_str());
    info.description = ReadIni(ini, L"description");
    info.source = ReadIni(ini, L"source");
    info.license = ReadIni(ini, L"license");
    info.required = builtIn && ReadIni(ini, L"required", L"0") == L"1";
    info.order = _wtoi(ReadIni(ini, L"order", L"100").c_str());
    out.push_back(std::move(info));
  }
  SortLexicons(out);
  return out;
}

bool WriteLexiconInfo(const std::wstring& iniPath, const LexiconInfo& info) {
  std::wstring text = L"\xFEFF[lexicon]\r\n";
  text += L"name=" + OneLine(info.name) + L"\r\n";
  text += L"description=" + OneLine(info.description) + L"\r\n";
  text += L"source=" + OneLine(info.source) + L"\r\n";
  text += L"license=" + OneLine(info.license) + L"\r\n";
  text += L"required=" + std::wstring(info.required ? L"1" : L"0") + L"\r\n";
  text += L"order=" + std::to_wstring(info.order) + L"\r\n";
  HANDLE file = CreateFileW(iniPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
  const bool ok = WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes;
  CloseHandle(file);
  return ok;
}

std::vector<LexiconInfo> EnabledLexicons(const std::vector<LexiconInfo>& all,
                                         const std::vector<std::wstring>& disabled) {
  std::vector<LexiconInfo> out;
  for (const LexiconInfo& info : all) {
    const bool off = std::find(disabled.begin(), disabled.end(), info.id) != disabled.end();
    if (info.required || !off) out.push_back(info);
  }
  return out;
}

}  // namespace llaime
