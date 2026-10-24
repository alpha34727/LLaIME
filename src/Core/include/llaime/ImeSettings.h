#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace llaime {

// Settings the TIP reads, in HKCU\Software\LLaIME (written by LLaIME.Settings):
//   DisabledLexicons  REG_MULTI_SZ  ids of lexicon layers turned off
//   UserLexiconDir    REG_SZ        where imported layers live (%LOCALAPPDATA%\LLaIME\lexicons;
//                                   a TIP in an AppContainer cannot resolve that folder itself)
//   Layout            REG_SZ        Bopomofo keyboard layout: "standard"
//   ContextChars      REG_DWORD     how many characters before the composition are given to
//                                   the ranker as context (0: none)
//   FollowingChars    REG_DWORD     how many characters after it (0: none; only the language
//                                   model uses them)
//   LlmWriting        REG_DWORD     0-100: how much the language model writes the conversion
//                                   itself rather than ranking the lexicon's best (0: ranking)
//   Generation        REG_DWORD     changes with every save; TIPs reload when it moves
// The key and the lexicon folder grant ALL APPLICATION PACKAGES read access, so the TIP works
// in packaged and UWP apps too. Engine settings (model, device) stay in engine.ini.
inline constexpr uint32_t kDefaultContextChars = 32;
inline constexpr uint32_t kMaxContextChars = 128;
// Each character after the composition is decoded once per candidate, so the default is short:
// 8 gets most of what 32 gains (tests/eval/RESULTS.md) at about half the extra latency.
inline constexpr uint32_t kDefaultFollowingChars = 8;
inline constexpr uint32_t kMaxLlmWriting = 100;

struct ImeSettings {
  std::vector<std::wstring> disabledLexicons;
  std::wstring userLexiconDir;
  std::wstring layout = L"standard";
  uint32_t contextChars = kDefaultContextChars;
  uint32_t followingChars = kDefaultFollowingChars;  // at most kMaxContextChars too
  uint32_t llmWriting = 0;                           // at most kMaxLlmWriting
  uint32_t generation = 0;
};

inline constexpr wchar_t kSettingsKey[] = L"Software\\LLaIME";

ImeSettings LoadImeSettings();
// Only the generation: cheap enough to check before every composition.
uint32_t ReadSettingsGeneration();
// Writes everything but `generation`, which is bumped. Creates the key with AppContainer read
// access if needed.
bool SaveImeSettings(const ImeSettings& settings);

// Gives ALL APPLICATION PACKAGES read access to a folder (and what it contains).
bool GrantAppContainerRead(const std::wstring& folder);

// %LOCALAPPDATA%\LLaIME (for full-trust processes: the engine and the settings app).
std::wstring UserDataDir();

}  // namespace llaime
