#pragma once
// Shared by the evaluation tools: UTF-8 conversion and the tests/eval TSV format
// (id, context, text, reading).
#include <windows.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "llaime/Syllable.h"

namespace llaime::eval {

inline std::string ToUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

inline std::wstring FromUtf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

struct Row {
  std::string id, context, text;
  std::vector<Syllable> reading;
};

inline std::vector<Row> LoadRows(const std::string& path, size_t limit) {
  std::vector<Row> rows;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line) && rows.size() < limit) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string col;
    while (std::getline(ss, col, '\t')) cols.push_back(col);
    if (cols.size() < 4) continue;
    Row row{cols[0], cols[1], cols[2], {}};
    std::wstringstream syllables(FromUtf8(cols[3]));
    std::wstring s;
    bool ok = true;
    while (syllables >> s) {
      auto parsed = Syllable::Parse(s);
      if (!parsed) {
        ok = false;
        break;
      }
      row.reading.push_back(*parsed);
    }
    if (ok) rows.push_back(std::move(row));
  }
  return rows;
}

inline std::string ExeDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p(path);
  return ToUtf8(p.substr(0, p.find_last_of(L"\\/")));
}

}  // namespace llaime::eval
