#pragma once
#include <string>
#include <vector>

namespace llaime {

// A lexicon layer on disk: <id>.bin with its description in <id>.ini next to it
// (UTF-16 with BOM, so GetPrivateProfileString reads Chinese names):
//   [lexicon]
//   name=McBopomofo 主詞庫
//   description=...
//   source=https://github.com/openvanilla/McBopomofo
//   license=MIT
//   required=1   ; always enabled (the main table)
//   order=0      ; lower first
// Built-in layers live in the install directory (<root>\data\lexicons); layers the user
// imported live in %LOCALAPPDATA%\LLaIME\lexicons.
struct LexiconInfo {
  std::wstring id;    // file stem, e.g. "mcbopomofo"
  std::wstring path;  // the .bin file
  std::wstring name, description, source, license;
  bool builtIn = false;   // shipped with LLaIME: can be disabled but not deleted
  bool required = false;  // always enabled
  int order = 100;
};

// Layers in `dir`, sorted (required first, then order, then name). A .bin without an .ini is
// listed under its file name.
std::vector<LexiconInfo> ListLexicons(const std::wstring& dir, bool builtIn);
void SortLexicons(std::vector<LexiconInfo>& lexicons);

// Writes <dir>\<id>.ini for `info` (path is not stored).
bool WriteLexiconInfo(const std::wstring& iniPath, const LexiconInfo& info);

// The layers to open: every required layer, and the others unless their id is in `disabled`.
std::vector<LexiconInfo> EnabledLexicons(const std::vector<LexiconInfo>& all,
                                         const std::vector<std::wstring>& disabled);

}  // namespace llaime
