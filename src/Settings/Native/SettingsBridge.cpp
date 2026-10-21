// LLaIME.SettingsNative.dll: what the settings app (C#, WinUI 3) needs from the IME's own
// code, as a flat C API. Lists come back as JSON (UTF-16, allocated with CoTaskMemAlloc, which
// .NET frees when it marshals a returned string).
//
//  - lexicon layers: list, turn on/off (HKCU\Software\LLaIME), import a file as a new layer,
//    delete an imported layer;
//  - the user dictionary in user.db (through UserStore, as the engine does): phrases, learned
//    picks, clearing what was learned, pausing learning, import/export;
//  - readings guessed from the lexicon for words people type in;
//  - the engine over its pipe: status, compute devices, reload after a settings change, and a
//    latency test.
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cwctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "Json.h"
#include "UserStore.h"
#include "llaime/ImeSettings.h"
#include "llaime/LexiconBuilder.h"
#include "llaime/LexiconCatalog.h"
#include "llaime/Protocol.h"
#include "llaime/UserDictionary.h"

#define LLAIME_API extern "C" __declspec(dllexport)

using namespace llaime;
namespace fs = std::filesystem;
namespace proto = llaime::protocol;

namespace {

struct Bridge {
  std::mutex mutex;
  fs::path installRoot;  // <root>\data\lexicons, <root>\engine\LLaIME.Engine.exe
  fs::path dataDir;      // %LOCALAPPDATA%\LLaIME
  std::unique_ptr<UserStore> store;
  std::unique_ptr<Lexicon> lexicon;  // every layer, on or off, for guessing readings
  std::unique_ptr<ReadingGuesser> guesser;
  uint32_t requestId = 1;
};

Bridge& B() {
  static Bridge bridge;
  return bridge;
}

wchar_t* Return(const std::wstring& s) {
  const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
  auto* out = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
  if (out) memcpy(out, s.c_str(), bytes);
  return out;
}

wchar_t* Return(const JsonWriter& json) { return Return(json.str()); }

wchar_t* Error(const std::wstring& message) {
  JsonWriter json;
  json.BeginObject().Field(L"ok", false).Field(L"error", message).EndObject();
  return Return(json);
}

fs::path UserLexiconDir() { return B().dataDir / L"lexicons"; }
fs::path BuiltInLexiconDir() { return B().installRoot / L"data" / L"lexicons"; }

std::vector<LexiconInfo> AllLexicons() {
  auto all = ListLexicons(BuiltInLexiconDir().wstring(), true);
  auto user = ListLexicons(UserLexiconDir().wstring(), false);
  all.insert(all.end(), user.begin(), user.end());
  return all;
}

// Saves the settings with the user lexicon folder filled in (the TIP cannot resolve it).
bool Save(ImeSettings settings) {
  settings.userLexiconDir = UserLexiconDir().wstring();
  return SaveImeSettings(settings);
}

UserStore* Store() {
  Bridge& b = B();
  if (!b.store) {
    auto store = std::make_unique<UserStore>();
    std::error_code ec;
    fs::create_directories(b.dataDir, ec);
    if (!store->Open((b.dataDir / L"user.db").wstring())) return nullptr;
    b.store = std::move(store);
  }
  return b.store.get();
}

const ReadingGuesser* Guesser() {
  Bridge& b = B();
  if (!b.guesser) {
    b.lexicon = std::make_unique<Lexicon>();
    for (const LexiconInfo& info : AllLexicons()) b.lexicon->AddLayer(info.path);
    b.guesser = std::make_unique<ReadingGuesser>(*b.lexicon);
  }
  return b.guesser.get();
}

// Layers were added or removed: guess from the new set next time.
void ResetGuesser() {
  B().guesser.reset();
  B().lexicon.reset();
}

proto::Reading Wire(std::span<const Syllable> reading) {
  proto::Reading out;
  for (Syllable s : reading) out.push_back(s.bits());
  return out;
}

std::vector<Syllable> FromWire(const proto::Reading& reading) {
  std::vector<Syllable> out;
  for (uint16_t bits : reading) out.push_back(Syllable(bits));
  return out;
}

// `key` is the stored form (FormatReading), as the lists return it.
// An empty key is a phrase typed by its shortcut alone.
std::optional<proto::Reading> FromKey(const wchar_t* key) {
  if (!key || !*key) return proto::Reading();
  auto parsed = ParseReading(key);
  if (!parsed) return std::nullopt;
  return Wire(*parsed);
}

std::optional<std::wstring> ReadText(const std::wstring& path) {
  std::ifstream in(fs::path(path), std::ios::binary);
  if (!in) return std::nullopt;
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFF &&
      static_cast<uint8_t>(bytes[1]) == 0xFE) {
    return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2),
                        (bytes.size() - 2) / sizeof(wchar_t));
  }
  size_t skip = bytes.size() >= 3 && bytes.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
  UINT codePage = CP_UTF8;
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + skip,
                              static_cast<int>(bytes.size() - skip), nullptr, 0);
  if (n == 0 && bytes.size() > skip) {
    codePage = 950;  // Big5: older Taiwanese word lists
    n = MultiByteToWideChar(codePage, 0, bytes.data() + skip,
                            static_cast<int>(bytes.size() - skip), nullptr, 0);
  }
  std::wstring out(n, L'\0');
  MultiByteToWideChar(codePage, 0, bytes.data() + skip, static_cast<int>(bytes.size() - skip),
                      out.data(), n);
  return out;
}

std::vector<std::wstring_view> Lines(std::wstring_view text) {
  std::vector<std::wstring_view> out;
  while (!text.empty()) {
    const size_t nl = text.find(L'\n');
    out.push_back(text.substr(0, nl));
    if (nl == std::wstring_view::npos) break;
    text.remove_prefix(nl + 1);
  }
  return out;
}

// Phrases read from a file, with readings guessed where the file has none.
struct Parsed {
  size_t lines = 0;
  size_t guessed = 0;
  std::vector<ImportedPhrase> phrases;
  std::vector<std::wstring> unreadable;  // texts without a reading
};

Parsed ParseFile(const std::wstring& text) {
  Parsed out;
  const ReadingGuesser* guesser = Guesser();
  for (std::wstring_view line : Lines(text)) {
    auto phrase = ParseLexiconLine(line);
    if (!phrase) continue;
    ++out.lines;
    if (phrase->reading.empty()) {
      auto guesses = guesser->Guess(phrase->text, 1);
      if (guesses.empty()) {
        out.unreadable.push_back(phrase->text);
        continue;
      }
      phrase->reading = std::move(guesses[0]);
      ++out.guessed;
    }
    if (phrase->reading.size() > Lexicon::kMaxPhraseLength) {
      out.unreadable.push_back(phrase->text);
      continue;
    }
    out.phrases.push_back(std::move(*phrase));
  }
  return out;
}

void WriteUnreadable(JsonWriter& json, const std::vector<std::wstring>& unreadable) {
  json.Field(L"unreadableCount", unreadable.size());
  json.Key(L"unreadable").BeginArray();
  for (size_t i = 0; i < unreadable.size() && i < 100; ++i) json.Value(unreadable[i]);
  json.EndArray();
}

// --- engine pipe ----------------------------------------------------------------------------

std::optional<std::vector<uint8_t>> Call(const std::vector<uint8_t>& request,
                                         DWORD timeoutMs = 3000) {
  const std::wstring name = proto::PipeName(proto::CurrentUserSid());
  if (!WaitNamedPipeW(name.c_str(), timeoutMs)) return std::nullopt;
  HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) return std::nullopt;
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
  std::vector<uint8_t> reply(64 * 1024);
  DWORD read = 0;
  BOOL ok = TransactNamedPipe(pipe, const_cast<uint8_t*>(request.data()),
                              static_cast<DWORD>(request.size()), reply.data(),
                              static_cast<DWORD>(reply.size()), &read, nullptr);
  size_t total = read;
  while (!ok && GetLastError() == ERROR_MORE_DATA) {
    reply.resize(total + 64 * 1024);
    ok = ReadFile(pipe, reply.data() + total, 64 * 1024, &read, nullptr);
    total += read;
  }
  CloseHandle(pipe);
  if (!ok) return std::nullopt;
  reply.resize(total);
  return reply;
}

template <typename T>
std::optional<T> CallFor(const std::vector<uint8_t>& request, proto::MessageType expected,
                         DWORD timeoutMs = 3000) {
  auto reply = Call(request, timeoutMs);
  if (!reply) return std::nullopt;
  proto::Reader in(reply->data(), reply->size());
  T out;
  if (in.Header() != expected || !proto::Decode(in, &out)) return std::nullopt;
  return out;
}

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5));
  return v[i];
}

std::wstring EngineIni() { return (B().dataDir / L"engine.ini").wstring(); }

}  // namespace

// Where LLaIME is installed (for the built-in lexicons and the engine). Call first. `dataDir`
// replaces %LOCALAPPDATA%\LLaIME when not empty (tests).
LLAIME_API wchar_t* LLaime_Init(const wchar_t* installRoot, const wchar_t* dataDir) {
  std::lock_guard<std::mutex> lock(B().mutex);
  B().installRoot = installRoot ? installRoot : L"";
  B().dataDir = dataDir && *dataDir ? fs::path(dataDir) : fs::path(UserDataDir());
  std::error_code ec;
  fs::create_directories(UserLexiconDir(), ec);
  // Imported layers must be readable by the TIP in packaged and UWP apps too.
  GrantAppContainerRead(UserLexiconDir().wstring());
  // Tell the TIPs where imported layers are (not for a test data folder).
  if (!(dataDir && *dataDir) && LoadImeSettings().userLexiconDir != UserLexiconDir().wstring()) {
    Save(LoadImeSettings());
  }
  JsonWriter json;
  json.BeginObject()
      .Field(L"ok", Store() != nullptr)
      .Field(L"dataDir", B().dataDir.wstring())
      .Field(L"engineIni", EngineIni())
      .Field(L"builtInLexicons", BuiltInLexiconDir().wstring())
      .Field(L"userLexicons", UserLexiconDir().wstring())
      .EndObject();
  return Return(json);
}

// --- lexicon layers -------------------------------------------------------------------------

LLAIME_API wchar_t* LLaime_ListLexicons() {
  std::lock_guard<std::mutex> lock(B().mutex);
  const ImeSettings settings = LoadImeSettings();
  JsonWriter json;
  json.BeginArray();
  for (const LexiconInfo& info : AllLexicons()) {
    LexiconLayer layer;
    const size_t entries = layer.Open(info.path) ? layer.entryCount() : 0;
    const bool disabled = std::find(settings.disabledLexicons.begin(),
                                    settings.disabledLexicons.end(),
                                    info.id) != settings.disabledLexicons.end();
    json.BeginObject()
        .Field(L"id", info.id)
        .Field(L"name", info.name)
        .Field(L"description", info.description)
        .Field(L"source", info.source)
        .Field(L"license", info.license)
        .Field(L"builtIn", info.builtIn)
        .Field(L"required", info.required)
        .Field(L"enabled", info.required || !disabled)
        .Field(L"entries", entries)
        .Field(L"path", info.path)
        .EndObject();
  }
  json.EndArray();
  return Return(json);
}

LLAIME_API int LLaime_SetLexiconEnabled(const wchar_t* id, int enabled) {
  std::lock_guard<std::mutex> lock(B().mutex);
  ImeSettings settings = LoadImeSettings();
  auto& off = settings.disabledLexicons;
  off.erase(std::remove(off.begin(), off.end(), std::wstring(id)), off.end());
  if (!enabled) off.push_back(id);
  return Save(settings) ? 1 : 0;
}

// What importing `path` would give, without writing anything.
LLAIME_API wchar_t* LLaime_PreviewImport(const wchar_t* path) {
  std::lock_guard<std::mutex> lock(B().mutex);
  auto text = ReadText(path);
  if (!text) return Error(L"無法讀取檔案");
  Parsed parsed = ParseFile(*text);
  JsonWriter json;
  json.BeginObject()
      .Field(L"ok", true)
      .Field(L"lines", parsed.lines)
      .Field(L"phrases", parsed.phrases.size())
      .Field(L"guessed", parsed.guessed);
  WriteUnreadable(json, parsed.unreadable);
  json.Key(L"sample").BeginArray();
  for (size_t i = 0; i < parsed.phrases.size() && i < 50; ++i) {
    json.BeginObject()
        .Field(L"text", parsed.phrases[i].text)
        .Field(L"reading", DisplayReading(parsed.phrases[i].reading))
        .EndObject();
  }
  json.EndArray().EndObject();
  return Return(json);
}

// Compiles `path` into a new layer in the user lexicon folder, turned on.
LLAIME_API wchar_t* LLaime_ImportLexicon(const wchar_t* path, const wchar_t* name,
                                         const wchar_t* description, const wchar_t* license) {
  std::lock_guard<std::mutex> lock(B().mutex);
  auto text = ReadText(path);
  if (!text) return Error(L"無法讀取檔案");
  Parsed parsed = ParseFile(*text);
  if (parsed.phrases.empty()) return Error(L"檔案中沒有可以標注音的詞");
  LexiconBuilder builder;
  for (const auto& p : parsed.phrases) {
    builder.Add(p.reading, p.text,
                p.score ? std::min(*p.score, 0.0f) : UnattestedScore(CodePoints(p.text).size()));
  }
  // user-<time>: unique, and never the id of a built-in layer.
  const std::wstring id = L"user-" + std::to_wstring(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const fs::path bin = UserLexiconDir() / (id + L".bin");
  LexiconInfo info;
  info.name = name && *name ? name : fs::path(path).stem().wstring();
  info.description = description ? description : L"";
  info.license = license ? license : L"";
  info.source = path;
  if (!builder.Write(bin.wstring()) ||
      !WriteLexiconInfo(fs::path(bin).replace_extension(L".ini").wstring(), info)) {
    std::error_code ec;
    fs::remove(bin, ec);
    return Error(L"無法寫入詞庫檔案");
  }
  ResetGuesser();
  Save(LoadImeSettings());  // new generation: TIPs pick the layer up
  JsonWriter json;
  json.BeginObject()
      .Field(L"ok", true)
      .Field(L"id", id)
      .Field(L"entries", builder.size());
  WriteUnreadable(json, parsed.unreadable);
  json.EndObject();
  return Return(json);
}

// Only imported layers can be deleted; built-in ones can only be turned off.
LLAIME_API int LLaime_DeleteLexicon(const wchar_t* id) {
  std::lock_guard<std::mutex> lock(B().mutex);
  for (const LexiconInfo& info : ListLexicons(UserLexiconDir().wstring(), false)) {
    if (info.id != id) continue;
    ResetGuesser();
    // TIPs map the file; take it out of their settings first, then remove it. If an app still
    // has it mapped, deletion waits until the app lets go (the file is marked for deletion).
    ImeSettings settings = LoadImeSettings();
    auto& off = settings.disabledLexicons;
    off.erase(std::remove(off.begin(), off.end(), info.id), off.end());
    std::error_code ec;
    const fs::path bin = info.path;
    const fs::path aside = fs::path(bin).replace_extension(L".deleted");
    fs::remove(fs::path(bin).replace_extension(L".ini"), ec);
    fs::rename(bin, aside, ec);
    Save(settings);
    if (!DeleteFileW(aside.c_str())) MoveFileExW(aside.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return 1;
  }
  return 0;
}

// --- readings -------------------------------------------------------------------------------

LLAIME_API wchar_t* LLaime_GuessReadings(const wchar_t* text) {
  std::lock_guard<std::mutex> lock(B().mutex);
  JsonWriter json;
  json.BeginArray();
  for (const auto& reading : Guesser()->Guess(text ? text : L"")) {
    json.Value(DisplayReading(reading));
  }
  json.EndArray();
  return Return(json);
}

// 1 if `reading` is a valid written reading for `text` (one syllable per character).
LLAIME_API int LLaime_CheckReading(const wchar_t* reading, const wchar_t* text) {
  auto parsed = ParseWrittenReading(reading ? reading : L"");
  if (!parsed || parsed->size() > Lexicon::kMaxPhraseLength) return 0;
  return CodePoints(text ? text : L"").size() == parsed->size() ? 1 : 0;
}

// --- user dictionary ------------------------------------------------------------------------

LLAIME_API wchar_t* LLaime_ListPhrases() {
  std::lock_guard<std::mutex> lock(B().mutex);
  JsonWriter json;
  json.BeginArray();
  if (UserStore* store = Store()) {
    for (const auto& row : store->ListPhrases()) {
      const auto reading = FromWire(row.reading);
      json.BeginObject()
          .Field(L"key", FormatReading(reading))
          .Field(L"reading", DisplayReading(reading))
          .Field(L"text", row.text)
          .Field(L"kind", row.kind)
          .Field(L"created", row.created)
          .Field(L"shortcut", row.shortcut)
          .EndObject();
    }
  }
  json.EndArray();
  return Return(json);
}

// Checks what the add / edit dialog entered: a written reading (may be empty when a shortcut
// is given) and a shortcut ("" or letters and digits, not used by another phrase). On success
// `*reading` and `*shortcut` hold the stored forms.
std::optional<std::wstring> CheckPhrase(UserStore* store, const wchar_t* written,
                                        const wchar_t* text, const wchar_t* shortcutIn,
                                        const std::wstring& ownText, proto::Reading* reading,
                                        std::wstring* shortcut) {
  shortcut->clear();
  for (const wchar_t* p = shortcutIn ? shortcutIn : L""; *p; ++p) {
    if (!iswspace(*p)) shortcut->push_back(static_cast<wchar_t>(towlower(*p)));
  }
  if (!shortcut->empty() && !UserDictionary::ValidShortcut(*shortcut)) {
    return L"快捷鍵只能用英文字母與數字（最多 16 個）";
  }
  const std::wstring w = written ? written : L"";
  if (w.find_first_not_of(L" 	") == std::wstring::npos) {
    if (shortcut->empty()) return L"請輸入注音，或指定快捷鍵";
    reading->clear();
  } else {
    auto parsed = ParseWrittenReading(w);
    if (!parsed || parsed->size() > Lexicon::kMaxPhraseLength) return L"注音格式不正確";
    *reading = Wire(*parsed);
  }
  if (!text || !*text) return L"請輸入詞";
  if (const std::wstring owner = store->ShortcutOwner(*shortcut);
      !owner.empty() && owner != ownText) {
    return L"快捷鍵「" + *shortcut + L"」已經用在「" + owner + L"」";
  }
  return std::nullopt;
}

wchar_t* Ok() {
  JsonWriter json;
  json.BeginObject().Field(L"ok", true).EndObject();
  return Return(json);
}

LLAIME_API wchar_t* LLaime_AddPhrase(const wchar_t* reading, const wchar_t* text,
                                     const wchar_t* shortcut) {
  std::lock_guard<std::mutex> lock(B().mutex);
  UserStore* store = Store();
  if (!store) return Error(L"無法開啟使用者詞庫");
  proto::Reading wire;
  std::wstring keys;
  if (auto error = CheckPhrase(store, reading, text, shortcut, text ? text : L"", &wire, &keys)) {
    return Error(*error);
  }
  if (!store->AddPhrase(wire, text, 0, keys)) return Error(L"無法加入這個詞");
  return Ok();
}

LLAIME_API wchar_t* LLaime_UpdatePhrase(const wchar_t* key, const wchar_t* text,
                                        const wchar_t* newReading, const wchar_t* newText,
                                        const wchar_t* newShortcut) {
  std::lock_guard<std::mutex> lock(B().mutex);
  UserStore* store = Store();
  auto old = FromKey(key);
  if (!store || !old) return Error(L"無法開啟使用者詞庫");
  proto::Reading wire;
  std::wstring keys;
  if (auto error = CheckPhrase(store, newReading, newText, newShortcut, text ? text : L"", &wire,
                               &keys)) {
    return Error(*error);
  }
  if (!store->UpdatePhrase(*old, text, wire, newText, keys)) return Error(L"無法儲存這個詞");
  return Ok();
}

LLAIME_API int LLaime_RemovePhrase(const wchar_t* key, const wchar_t* text) {
  std::lock_guard<std::mutex> lock(B().mutex);
  auto reading = FromKey(key);
  UserStore* store = Store();
  return reading && store && store->RemovePhrase(*reading, text) ? 1 : 0;
}

LLAIME_API int LLaime_MakeManual(const wchar_t* key, const wchar_t* text) {
  std::lock_guard<std::mutex> lock(B().mutex);
  auto reading = FromKey(key);
  UserStore* store = Store();
  return reading && store && store->MakeManual(*reading, text) ? 1 : 0;
}

LLAIME_API wchar_t* LLaime_ListSelections() {
  std::lock_guard<std::mutex> lock(B().mutex);
  JsonWriter json;
  json.BeginArray();
  if (UserStore* store = Store()) {
    for (const auto& row : store->ListSelections()) {
      const auto reading = FromWire(row.reading);
      json.BeginObject()
          .Field(L"key", FormatReading(reading))
          .Field(L"reading", DisplayReading(reading))
          .Field(L"text", row.text)
          .Field(L"prev", row.prev)
          .Field(L"weight", row.weight)
          .Field(L"updated", row.updated)
          .EndObject();
    }
  }
  json.EndArray();
  return Return(json);
}

LLAIME_API int LLaime_RemoveSelection(const wchar_t* key, const wchar_t* text,
                                      const wchar_t* prev) {
  std::lock_guard<std::mutex> lock(B().mutex);
  auto reading = FromKey(key);
  UserStore* store = Store();
  return reading && store && store->RemoveSelection(*reading, text, prev ? prev : L"") ? 1 : 0;
}

LLAIME_API int LLaime_GetContextChars() {
  return static_cast<int>(LoadImeSettings().contextChars);
}

LLAIME_API int LLaime_SetContextChars(int chars) {
  std::lock_guard<std::mutex> lock(B().mutex);
  ImeSettings settings = LoadImeSettings();
  settings.contextChars = static_cast<uint32_t>(chars < 0 ? 0 : chars);
  return Save(settings) ? 1 : 0;
}

LLAIME_API int LLaime_GetFollowingChars() {
  return static_cast<int>(LoadImeSettings().followingChars);
}

LLAIME_API int LLaime_SetFollowingChars(int chars) {
  std::lock_guard<std::mutex> lock(B().mutex);
  ImeSettings settings = LoadImeSettings();
  settings.followingChars = static_cast<uint32_t>(chars < 0 ? 0 : chars);
  return Save(settings) ? 1 : 0;
}

LLAIME_API int LLaime_ClearLearning() {
  std::lock_guard<std::mutex> lock(B().mutex);
  UserStore* store = Store();
  return store && store->ClearLearning() ? 1 : 0;
}

LLAIME_API int LLaime_GetLearningEnabled() {
  std::lock_guard<std::mutex> lock(B().mutex);
  UserStore* store = Store();
  return store && store->LearningEnabled() ? 1 : 0;
}

LLAIME_API int LLaime_SetLearningEnabled(int enabled) {
  std::lock_guard<std::mutex> lock(B().mutex);
  UserStore* store = Store();
  return store && store->SetLearningEnabled(enabled != 0) ? 1 : 0;
}

// Adds every phrase of a lexicon file or word list as a manual phrase.
LLAIME_API wchar_t* LLaime_ImportPhrases(const wchar_t* path) {
  std::lock_guard<std::mutex> lock(B().mutex);
  auto text = ReadText(path);
  if (!text) return Error(L"無法讀取檔案");
  UserStore* store = Store();
  if (!store) return Error(L"無法開啟使用者詞庫");
  Parsed parsed = ParseFile(*text);
  std::vector<std::pair<proto::Reading, std::wstring>> phrases;
  for (const auto& p : parsed.phrases) phrases.emplace_back(Wire(p.reading), p.text);
  const size_t added = store->AddPhrases(phrases);
  JsonWriter json;
  json.BeginObject()
      .Field(L"ok", true)
      .Field(L"phrases", parsed.phrases.size())
      .Field(L"added", added)
      .Field(L"guessed", parsed.guessed);
  WriteUnreadable(json, parsed.unreadable);
  json.EndObject();
  return Return(json);
}

// Writes the user's phrases as UTF-8 "text reading" lines (McBopomofo's user phrase format,
// which ParseLexiconLine reads back). Returns how many, or -1.
LLAIME_API int LLaime_ExportPhrases(const wchar_t* path, int includeLearned) {
  std::lock_guard<std::mutex> lock(B().mutex);
  UserStore* store = Store();
  if (!store) return -1;
  std::wstring out = L"# LLaIME 使用者詞庫：詞 注音（McBopomofo 使用者詞彙格式）\n";
  int count = 0;
  for (const auto& row : store->ListPhrases()) {
    if (row.kind != 0 && !includeLearned) continue;
    if (row.reading.empty()) continue;  // typed by shortcut only: the format has no place for it
    std::wstring reading;
    for (Syllable s : FromWire(row.reading)) {
      if (!reading.empty()) reading += L'-';
      reading += s.ToString();
    }
    out += row.text + L' ' + reading + L'\n';
    ++count;
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, out.data(), static_cast<int>(out.size()),
                                    nullptr, 0, nullptr, nullptr);
  std::string utf8(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, out.data(), static_cast<int>(out.size()), utf8.data(), n,
                      nullptr, nullptr);
  std::ofstream file(fs::path(path), std::ios::binary);
  if (!file.write(utf8.data(), utf8.size())) return -1;
  return count;
}

// --- engine ---------------------------------------------------------------------------------

LLAIME_API wchar_t* LLaime_EngineStatus() {
  auto status = CallFor<proto::StatusResponse>(proto::EncodeStatusRequest(),
                                               proto::MessageType::StatusResponse, 500);
  JsonWriter json;
  json.BeginObject().Field(L"running", status.has_value());
  if (status) {
    static const wchar_t* kStates[] = {L"loading", L"ready", L"failed"};
    json.Field(L"state", kStates[status->state])
        .Field(L"model", status->model)
        .Field(L"device", status->device)
        .Field(L"ranker", status->ranker)
        .Field(L"error", status->error);
  }
  json.EndObject();
  return Return(json);
}

LLAIME_API wchar_t* LLaime_EngineDevices() {
  auto devices = CallFor<proto::DevicesResponse>(proto::EncodeDevicesRequest(),
                                                 proto::MessageType::DevicesResponse);
  if (!devices) return Return(L"null");
  JsonWriter json;
  json.BeginArray();
  for (const auto& d : devices->devices) {
    json.BeginObject()
        .Field(L"name", d.name)
        .Field(L"description", d.description)
        .Field(L"memory", d.memory)
        .Field(L"gpu", d.gpu)
        .EndObject();
  }
  json.EndArray();
  return Return(json);
}

// Starts the installed engine if it is not running.
LLAIME_API int LLaime_StartEngine() {
  if (Call(proto::EncodeStatusRequest(), 200)) return 1;
  const fs::path exe = B().installRoot / L"engine" / L"LLaIME.Engine.exe";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, exe.parent_path().c_str(),
                      &si, &pi)) {
    return 0;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 1;
}

// Has the engine reread engine.ini and load the ranker again.
LLAIME_API int LLaime_EngineReload() {
  auto ack = CallFor<proto::Ack>(proto::EncodeReloadRequest(), proto::MessageType::Ack);
  return ack && ack->ok ? 1 : 0;
}

// Times `count` rank requests like the ones typing sends (8 candidates after some context).
// Sent as private requests, which the engine never caches.
LLAIME_API wchar_t* LLaime_EngineBenchmark(int count) {
  static const wchar_t* kCandidates[] = {L"天氣很好", L"天氣很號", L"天起很好", L"添妻很好",
                                         L"天氣恨好", L"田七很好", L"天器很好", L"天棄很好"};
  std::vector<double> times;
  for (int i = 0; i < std::max(1, count); ++i) {
    proto::RankRequest request;
    request.requestId = B().requestId++;
    request.privateMode = true;
    request.context = L"今天早上出門的時候，我發現";
    request.candidates.assign(std::begin(kCandidates), std::end(kCandidates));
    const auto start = std::chrono::steady_clock::now();
    auto response = CallFor<proto::RankResponse>(proto::Encode(request),
                                                 proto::MessageType::RankResponse, 2000);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start).count();
    if (!response || !response->ok) return Error(L"引擎沒有回應或模型尚未載入");
    if (i > 0) times.push_back(ms);  // the first may still warm up
  }
  double mean = 0;
  for (double t : times) mean += t;
  mean = times.empty() ? 0 : mean / times.size();
  JsonWriter json;
  json.BeginObject()
      .Field(L"ok", true)
      .Field(L"count", times.size())
      .Field(L"mean", mean)
      .Field(L"p50", Percentile(times, 0.5))
      .Field(L"p95", Percentile(times, 0.95))
      .EndObject();
  return Return(json);
}

// engine.ini values. The file is created as UTF-16 so model paths with Chinese survive.
LLAIME_API wchar_t* LLaime_ReadEngineSetting(const wchar_t* section, const wchar_t* key) {
  wchar_t buf[2048];
  GetPrivateProfileStringW(section, key, L"", buf, 2048, EngineIni().c_str());
  return Return(std::wstring(buf));
}

LLAIME_API int LLaime_WriteEngineSetting(const wchar_t* section, const wchar_t* key,
                                         const wchar_t* value) {
  const std::wstring ini = EngineIni();
  if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::error_code ec;
    fs::create_directories(B().dataDir, ec);
    HANDLE file = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      const wchar_t bom = 0xFEFF;
      DWORD written = 0;
      WriteFile(file, &bom, sizeof(bom), &written, nullptr);
      CloseHandle(file);
    }
  }
  return WritePrivateProfileStringW(section, key, value, ini.c_str()) ? 1 : 0;
}
