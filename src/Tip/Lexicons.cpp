// The lexicon layers, on the TIP side. Built-in layers are found next to the installed DLL
// (<root>\x64\LLaIME.Tip.dll -> <root>\data\lexicons), imported ones in the folder the
// settings name; the settings also say which layers are turned off. One Lexicon is shared by
// the text services of a process and rebuilt when the settings' generation moves; each text
// service switches to it between compositions.
#include <filesystem>
#include <mutex>

#include "Globals.h"
#include "TextService.h"
#include "llaime/ImeSettings.h"
#include "llaime/LexiconCatalog.h"

namespace llaime {
namespace {

namespace fs = std::filesystem;

// Checking the registry before every composition is cheap, but once a second is enough.
constexpr ULONGLONG kCheckIntervalMs = 1000;

struct SharedLexicon {
  std::mutex mutex;
  bool loaded = false;
  uint32_t generation = 0;
  ULONGLONG checkedTick = 0;
  std::shared_ptr<const Lexicon> lexicon;
  uint32_t contextChars = kDefaultContextChars;
};

SharedLexicon& Shared() {
  static SharedLexicon shared;
  return shared;
}

fs::path DllDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(g_hInst, path, MAX_PATH);
  return fs::path(path).parent_path();
}

std::shared_ptr<const Lexicon> Load(const ImeSettings& settings) {
  std::vector<LexiconInfo> all;
  const fs::path dll = DllDir();
  for (const fs::path& dir :
       {dll.parent_path() / L"data" / L"lexicons", dll / L"data" / L"lexicons"}) {
    all = ListLexicons(dir.wstring(), true);
    if (!all.empty()) break;
  }
  if (!settings.userLexiconDir.empty()) {
    auto user = ListLexicons(settings.userLexiconDir, false);
    all.insert(all.end(), user.begin(), user.end());
  }
  auto lexicon = std::make_shared<Lexicon>();
  for (const LexiconInfo& info : EnabledLexicons(all, settings.disabledLexicons)) {
    lexicon->AddLayer(info.path);
  }
  return lexicon->IsOpen() ? lexicon : nullptr;
}

}  // namespace

std::shared_ptr<const Lexicon> CurrentLexicon() {
  SharedLexicon& shared = Shared();
  std::lock_guard<std::mutex> lock(shared.mutex);
  const ULONGLONG now = GetTickCount64();
  if (shared.loaded && now - shared.checkedTick < kCheckIntervalMs) return shared.lexicon;
  shared.checkedTick = now;
  const uint32_t generation = ReadSettingsGeneration();
  if (shared.loaded && generation == shared.generation) return shared.lexicon;
  shared.contextChars = LoadImeSettings().contextChars;
  // Keep the old layers if the new set cannot be opened (e.g. a file being replaced).
  if (auto lexicon = Load(LoadImeSettings()); lexicon || !shared.loaded) {
    shared.lexicon = std::move(lexicon);
  }
  shared.loaded = true;
  shared.generation = generation;
  return shared.lexicon;
}

uint32_t CurrentContextChars() {
  CurrentLexicon();  // rereads the settings when they changed
  SharedLexicon& shared = Shared();
  std::lock_guard<std::mutex> lock(shared.mutex);
  return shared.contextChars;
}

void TextService::ApplyLexicon() {
  if (IsComposing()) return;
  auto lexicon = CurrentLexicon();
  if (lexicon == lexiconRef_) return;
  lexiconRef_ = std::move(lexicon);
  lexicon_ = lexiconRef_.get();
  composer_.SetLexicon(lexicon_);
}

}  // namespace llaime
