#include "UserStore.h"

#include <windows.h>

#include <sqlite3.h>

#include <chrono>
#include <cstdio>
#include <optional>
#include <vector>

#include "llaime/UserDictionary.h"

namespace llaime {
namespace {

constexpr size_t kMaxTextLength = 32;

// A prepared statement, finalized on scope exit. Text is bound and read as UTF-16.
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      fprintf(stderr, "user.db: %s\n", sqlite3_errmsg(db));
    }
  }
  ~Stmt() { sqlite3_finalize(stmt_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  explicit operator bool() const { return stmt_ != nullptr; }
  Stmt& Bind(int i, const std::wstring& s) {
    sqlite3_bind_text16(stmt_, i, s.data(), static_cast<int>(s.size() * sizeof(wchar_t)),
                        SQLITE_TRANSIENT);
    return *this;
  }
  Stmt& Bind(int i, int64_t v) {
    sqlite3_bind_int64(stmt_, i, v);
    return *this;
  }
  Stmt& Bind(int i, double v) {
    sqlite3_bind_double(stmt_, i, v);
    return *this;
  }
  bool Step() { return sqlite3_step(stmt_) == SQLITE_ROW; }
  bool Run() {
    const int rc = sqlite3_step(stmt_);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
      fprintf(stderr, "user.db: %s\n", sqlite3_errmsg(sqlite3_db_handle(stmt_)));
    }
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
  }
  std::wstring Text(int col) const {
    auto* p = static_cast<const wchar_t*>(sqlite3_column_text16(stmt_, col));
    return p ? std::wstring(p, sqlite3_column_bytes16(stmt_, col) / sizeof(wchar_t))
             : std::wstring();
  }
  int64_t Int(int col) const { return sqlite3_column_int64(stmt_, col); }
  double Real(int col) const { return sqlite3_column_double(stmt_, col); }

 private:
  sqlite3_stmt* stmt_ = nullptr;
};

// Weight decayed from `updated` to `now` (both Unix seconds), as SQL.
#define DECAYED(now) "weight * pow(0.5, (" now " - updated) / 86400.0 / ?9)"

// Validated stored form of a reading from the wire. Empty only if `allowEmpty` (a phrase
// typed by its shortcut alone).
std::optional<std::wstring> ReadingText(const protocol::Reading& reading,
                                        bool allowEmpty = false) {
  if (reading.empty() && allowEmpty) return std::wstring();
  if (reading.empty() || reading.size() > Lexicon::kMaxPhraseLength) return std::nullopt;
  std::vector<Syllable> syllables;
  for (uint16_t bits : reading) {
    Syllable s(bits);
    if (s.empty()) return std::nullopt;
    syllables.push_back(s);
  }
  return FormatReading(syllables);
}

bool ValidShortcut(const std::wstring& shortcut) {
  return shortcut.empty() || UserDictionary::ValidShortcut(shortcut);
}

bool ValidText(const std::wstring& text) {
  return !text.empty() && text.size() <= kMaxTextLength &&
         text.find_first_of(L"\r\n\t") == std::wstring::npos;
}

protocol::Reading WireReading(const std::wstring& text) {
  protocol::Reading out;
  if (auto syllables = ParseReading(text)) {
    for (Syllable s : *syllables) out.push_back(s.bits());
  }
  return out;
}

}  // namespace

UserStore::~UserStore() {
  if (db_) sqlite3_close_v2(db_);
}

int64_t UserStore::Now(int64_t now) {
  if (now) return now;
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool UserStore::Exec(const char* sql) {
  return sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool UserStore::Open(const std::wstring& path, std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sqlite3_open16(path.c_str(), &db_) != SQLITE_OK) {
    if (error) *error = db_ ? sqlite3_errmsg(db_) : "cannot open";
    sqlite3_close_v2(db_);
    db_ = nullptr;
    return false;
  }
  sqlite3_busy_timeout(db_, 2000);
  const bool ok =
      Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;") &&
      Exec("CREATE TABLE IF NOT EXISTS phrases(reading TEXT NOT NULL, text TEXT NOT NULL,"
           " kind INTEGER NOT NULL, created INTEGER NOT NULL, PRIMARY KEY(reading, text));"
           "CREATE TABLE IF NOT EXISTS selections(reading TEXT NOT NULL, text TEXT NOT NULL,"
           " prev TEXT NOT NULL, weight REAL NOT NULL, updated INTEGER NOT NULL,"
           " PRIMARY KEY(reading, text, prev));"
           "CREATE TABLE IF NOT EXISTS phrase_runs(reading TEXT NOT NULL, text TEXT NOT NULL,"
           " count INTEGER NOT NULL, updated INTEGER NOT NULL, PRIMARY KEY(reading, text));"
           "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);"
           "PRAGMA user_version=1;");
  if (!ok) {
    if (error) *error = sqlite3_errmsg(db_);
    sqlite3_close_v2(db_);
    db_ = nullptr;
    return false;
  }
  // Version 2: phrases typed by shortcut.
  {
    Stmt has(db_, "SELECT 1 FROM pragma_table_info('phrases') WHERE name = 'shortcut'");
    if (!has.Step() &&
        !Exec("ALTER TABLE phrases ADD COLUMN shortcut TEXT NOT NULL DEFAULT ''")) {
      if (error) *error = sqlite3_errmsg(db_);
      sqlite3_close_v2(db_);
      db_ = nullptr;
      return false;
    }
  }
  Exec("CREATE UNIQUE INDEX IF NOT EXISTS phrases_shortcut ON phrases(shortcut)"
       " WHERE shortcut <> ''; PRAGMA user_version=2;");
  // Forget what has faded away.
  Stmt prune(db_, "DELETE FROM selections WHERE " DECAYED("?1") " < ?2");
  prune.Bind(1, Now(0)).Bind(2, UserDictionary::kMinWeight).Bind(9, kHalfLifeDays).Run();
  // Versions from a previous run of the engine must not look current.
  version_ = GetTickCount() | 1;
  CheckOutsideChanges();
  return true;
}

void UserStore::CheckOutsideChanges() {
  Stmt q(db_, "PRAGMA data_version");
  if (!q.Step()) return;
  const int64_t v = q.Int(0);
  if (dataVersion_ >= 0 && v != dataVersion_) Bump();
  dataVersion_ = v;
}

uint32_t UserStore::Version() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (db_) CheckOutsideChanges();
  return version_;
}

bool UserStore::LearningEnabled() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;
  Stmt q(db_, "SELECT value FROM settings WHERE key = 'learning'");
  return !q.Step() || q.Text(0) != L"0";
}

bool UserStore::SetLearningEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;
  Stmt q(db_, "INSERT OR REPLACE INTO settings(key, value) VALUES('learning', ?1)");
  if (!q.Bind(1, std::wstring(enabled ? L"1" : L"0")).Run()) return false;
  Bump();
  CheckOutsideChanges();
  return true;
}

protocol::UserDictResponse UserStore::Snapshot(uint32_t knownVersion, int64_t now) {
  protocol::UserDictResponse r;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return r;
  CheckOutsideChanges();
  r.version = version_;
  if (knownVersion == version_) return r;
  r.changed = true;
  {
    Stmt q(db_, "SELECT value FROM settings WHERE key = 'learning'");
    r.learningEnabled = !q.Step() || q.Text(0) != L"0";
  }
  Stmt phrases(db_, "SELECT reading, text, kind, shortcut FROM phrases");
  while (phrases.Step()) {
    protocol::UserDictResponse::Phrase p{WireReading(phrases.Text(0)), phrases.Text(1),
                                          static_cast<uint32_t>(phrases.Int(2))};
    if (std::wstring keys = phrases.Text(3); !keys.empty()) r.shortcuts.push_back({keys, p.text});
    if (!p.reading.empty()) r.phrases.push_back(std::move(p));
  }
  Stmt selections(db_,
                  "SELECT reading, text, prev, " DECAYED("?1") " AS w FROM selections"
                  " WHERE w >= ?2 ORDER BY w DESC LIMIT ?3");
  selections.Bind(1, Now(now))
      .Bind(2, UserDictionary::kMinWeight)
      .Bind(3, static_cast<int64_t>(kMaxSelections))
      .Bind(9, kHalfLifeDays);
  while (selections.Step()) {
    protocol::UserDictResponse::Selection s{WireReading(selections.Text(0)), selections.Text(1),
                                             selections.Text(2), selections.Real(3)};
    if (!s.reading.empty()) r.selections.push_back(std::move(s));
  }
  return r;
}

bool UserStore::Learn(const protocol::LearnRequest& request, int64_t now) {
  if (!LearningEnabled()) return true;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;
  now = Now(now);
  if (!Exec("BEGIN IMMEDIATE")) return false;
  bool ok = true;
  for (const auto& s : request.selections) {
    const auto reading = ReadingText(s.reading);
    if (!reading || !ValidText(s.text) || s.prev.size() > kMaxTextLength) continue;
    // The overall record, and the one after the preceding word.
    std::vector<std::wstring> contexts = {std::wstring()};
    if (!s.prev.empty()) contexts.push_back(s.prev);
    for (const std::wstring& prev : contexts) {
      Stmt rivals(db_,
                  "UPDATE selections SET weight = " DECAYED("?1") " * 0.5, updated = ?1"
                  " WHERE reading = ?2 AND prev = ?3 AND text <> ?4");
      ok &= rivals.Bind(1, now).Bind(2, *reading).Bind(3, prev).Bind(4, s.text)
                .Bind(9, kHalfLifeDays).Run();
      Stmt pick(db_,
                "INSERT INTO selections(reading, text, prev, weight, updated)"
                " VALUES(?2, ?4, ?3, 1.0, ?1)"
                " ON CONFLICT(reading, text, prev) DO UPDATE SET"
                " weight = " DECAYED("?1") " + 1.0, updated = ?1");
      ok &= pick.Bind(1, now).Bind(2, *reading).Bind(3, prev).Bind(4, s.text)
                .Bind(9, kHalfLifeDays).Run();
    }
  }
  for (const auto& p : request.phrases) {
    const auto reading = ReadingText(p.reading);
    if (!reading || !ValidText(p.text)) continue;
    Stmt run(db_,
             "INSERT INTO phrase_runs(reading, text, count, updated) VALUES(?1, ?2, 1, ?3)"
             " ON CONFLICT(reading, text) DO UPDATE SET count = count + 1, updated = ?3"
             " RETURNING count");
    run.Bind(1, *reading).Bind(2, p.text).Bind(3, now);
    if (!run.Step()) {
      ok = false;
      continue;
    }
    const int64_t count = run.Int(0);
    run.Run();  // finish the statement
    if (count < kPromoteAfter) continue;
    Stmt promote(db_,
                 "INSERT OR IGNORE INTO phrases(reading, text, kind, created)"
                 " VALUES(?1, ?2, 1, ?3)");
    ok &= promote.Bind(1, *reading).Bind(2, p.text).Bind(3, now).Run();
    Stmt done(db_, "DELETE FROM phrase_runs WHERE reading = ?1 AND text = ?2");
    ok &= done.Bind(1, *reading).Bind(2, p.text).Run();
  }
  if (!ok || !Exec("COMMIT")) {
    Exec("ROLLBACK");
    return false;
  }
  Bump();
  CheckOutsideChanges();
  return true;
}

bool UserStore::AddPhrase(const protocol::Reading& reading, const std::wstring& text,
                          int64_t now, const std::wstring& shortcut) {
  const auto stored = ReadingText(reading, !shortcut.empty());
  if (!stored || !ValidText(text) || !ValidShortcut(shortcut)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;
  // A phrase added by hand stays manual even if it was learned before; a shortcut given here
  // replaces its old one.
  Stmt q(db_,
         "INSERT INTO phrases(reading, text, kind, created, shortcut) VALUES(?1, ?2, 0, ?3, ?4)"
         " ON CONFLICT(reading, text) DO UPDATE SET kind = 0,"
         " shortcut = CASE WHEN ?4 <> '' THEN ?4 ELSE shortcut END");
  if (!q.Bind(1, *stored).Bind(2, text).Bind(3, Now(now)).Bind(4, shortcut).Run()) {
    return false;
  }
  Bump();
  CheckOutsideChanges();
  return true;
}

bool UserStore::RemovePhrase(const protocol::Reading& reading, const std::wstring& text) {
  const auto stored = ReadingText(reading, true);
  if (!stored) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;
  Stmt q(db_, "DELETE FROM phrases WHERE reading = ?1 AND text = ?2");
  if (!q.Bind(1, *stored).Bind(2, text).Run()) return false;
  Bump();
  CheckOutsideChanges();
  return true;
}

std::vector<UserStore::PhraseRow> UserStore::ListPhrases() {
  std::vector<PhraseRow> out;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return out;
  Stmt q(db_,
         "SELECT reading, text, kind, created, shortcut FROM phrases"
         " ORDER BY created DESC, text");
  while (q.Step()) {
    PhraseRow row{WireReading(q.Text(0)), q.Text(1), static_cast<uint32_t>(q.Int(2)), q.Int(3),
                  q.Text(4)};
    if (!row.reading.empty() || !row.shortcut.empty()) out.push_back(std::move(row));
  }
  return out;
}

std::vector<UserStore::SelectionRow> UserStore::ListSelections(int64_t now) {
  std::vector<SelectionRow> out;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return out;
  Stmt q(db_,
         "SELECT reading, text, prev, " DECAYED("?1") " AS w, updated FROM selections"
         " WHERE w >= ?2 ORDER BY w DESC");
  q.Bind(1, Now(now)).Bind(2, UserDictionary::kMinWeight).Bind(9, kHalfLifeDays);
  while (q.Step()) {
    SelectionRow row{WireReading(q.Text(0)), q.Text(1), q.Text(2), q.Real(3), q.Int(4)};
    if (!row.reading.empty()) out.push_back(std::move(row));
  }
  return out;
}

bool UserStore::UpdatePhrase(const protocol::Reading& reading, const std::wstring& text,
                             const protocol::Reading& newReading, const std::wstring& newText,
                             const std::wstring& newShortcut) {
  const auto stored = ReadingText(reading, true);
  const auto updated = ReadingText(newReading, !newShortcut.empty());
  if (!stored || !updated || !ValidText(newText) || !ValidShortcut(newShortcut)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || !Exec("BEGIN IMMEDIATE")) return false;
  int64_t created = Now(0);
  {
    Stmt q(db_, "SELECT created FROM phrases WHERE reading = ?1 AND text = ?2");
    if (q.Bind(1, *stored).Bind(2, text).Step()) created = q.Int(0);
  }
  Stmt remove(db_, "DELETE FROM phrases WHERE reading = ?1 AND text = ?2");
  Stmt add(db_,
           "INSERT INTO phrases(reading, text, kind, created, shortcut) VALUES(?1, ?2, 0, ?3, ?4)"
           " ON CONFLICT(reading, text) DO UPDATE SET kind = 0, shortcut = ?4");
  const bool ok = remove.Bind(1, *stored).Bind(2, text).Run() &&
                  add.Bind(1, *updated).Bind(2, newText).Bind(3, created).Bind(4, newShortcut)
                      .Run();
  if (!ok || !Exec("COMMIT")) {
    Exec("ROLLBACK");
    return false;
  }
  Bump();
  CheckOutsideChanges();
  return true;
}

std::wstring UserStore::ShortcutOwner(const std::wstring& shortcut) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || shortcut.empty()) return {};
  Stmt q(db_, "SELECT text FROM phrases WHERE shortcut = ?1");
  return q.Bind(1, shortcut).Step() ? q.Text(0) : std::wstring();
}

bool UserStore::MakeManual(const protocol::Reading& reading, const std::wstring& text) {
  const auto stored = ReadingText(reading);
  if (!stored) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;
  Stmt q(db_, "UPDATE phrases SET kind = 0 WHERE reading = ?1 AND text = ?2");
  if (!q.Bind(1, *stored).Bind(2, text).Run()) return false;
  Bump();
  CheckOutsideChanges();
  return true;
}

size_t UserStore::AddPhrases(
    const std::vector<std::pair<protocol::Reading, std::wstring>>& phrases, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_ || !Exec("BEGIN IMMEDIATE")) return 0;
  now = Now(now);
  size_t added = 0;
  for (const auto& [reading, text] : phrases) {
    const auto stored = ReadingText(reading);
    if (!stored || !ValidText(text)) continue;
    Stmt q(db_,
           "INSERT INTO phrases(reading, text, kind, created) VALUES(?1, ?2, 0, ?3)"
           " ON CONFLICT(reading, text) DO UPDATE SET kind = 0");
    if (q.Bind(1, *stored).Bind(2, text).Bind(3, now).Run()) {
      added += sqlite3_changes(db_) > 0 ? 1 : 0;
    }
  }
  if (!Exec("COMMIT")) {
    Exec("ROLLBACK");
    return 0;
  }
  Bump();
  CheckOutsideChanges();
  return added;
}

bool UserStore::RemoveSelection(const protocol::Reading& reading, const std::wstring& text,
                                const std::wstring& prev) {
  const auto stored = ReadingText(reading);
  if (!stored) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;
  Stmt q(db_, "DELETE FROM selections WHERE reading = ?1 AND text = ?2 AND prev = ?3");
  if (!q.Bind(1, *stored).Bind(2, text).Bind(3, prev).Run()) return false;
  Bump();
  CheckOutsideChanges();
  return true;
}

bool UserStore::ClearLearning() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!db_) return false;
  // secure_delete overwrites what is deleted, and the checkpoint empties the write-ahead
  // log, so nothing of it is left in the files either.
  Exec("PRAGMA secure_delete=ON");
  const bool ok = Exec("BEGIN IMMEDIATE; DELETE FROM selections; DELETE FROM phrase_runs;"
                       " DELETE FROM phrases WHERE kind = 1; COMMIT;");
  if (!ok) Exec("ROLLBACK");
  Exec("PRAGMA secure_delete=OFF");
  if (!ok) return false;
  Exec("PRAGMA wal_checkpoint(TRUNCATE)");
  Bump();
  CheckOutsideChanges();
  return true;
}

}  // namespace llaime
