#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "llaime/Protocol.h"

struct sqlite3;

namespace llaime {

// The user's phrases and what they taught by picking candidates, in
// %LOCALAPPDATA%\LLaIME\user.db (SQLite, WAL). The engine is its only writer among the IME's
// processes; the settings app may edit it too, which shows up as a new version.
//
//   phrases     (reading, text, kind 0 manual / 1 learned, created, shortcut): shortcut is
//               '' or the keys that type the phrase after ` p (unique); a phrase with a
//               shortcut may have no reading ('').
//   selections  (reading, text, prev, weight, updated): picks of `text` for `reading`, overall
//               (prev = '') and after the word `prev`. A pick adds 1 and halves the other texts
//               picked for the same reading; weights halve every kHalfLifeDays.
//   phrase_runs (reading, text, count, updated): characters picked one by one in a row; seen
//               kPromoteAfter times, the run becomes a learned phrase.
//   settings    (key, value): learning = 0 / 1
// Readings are stored as text (llaime::FormatReading), e.g. ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ.
class UserStore {
 public:
  static constexpr double kHalfLifeDays = 30;
  static constexpr int kPromoteAfter = 2;
  // Sent to the TIPs, heaviest first: bounds the snapshot every TIP fetches after a commit.
  static constexpr size_t kMaxSelections = 20'000;

  UserStore() = default;
  ~UserStore();
  UserStore(const UserStore&) = delete;
  UserStore& operator=(const UserStore&) = delete;

  bool Open(const std::wstring& path, std::string* error = nullptr);
  bool IsOpen() const { return db_ != nullptr; }

  // Changes with every edit, here or by another process. Never 0.
  uint32_t Version();
  bool LearningEnabled();
  bool SetLearningEnabled(bool enabled);

  // The whole dictionary, unless `knownVersion` is current. `now`: Unix seconds (0: clock).
  protocol::UserDictResponse Snapshot(uint32_t knownVersion, int64_t now = 0);
  // Ignored (true) while learning is paused.
  bool Learn(const protocol::LearnRequest& request, int64_t now = 0);
  // `reading` may be empty when `shortcut` is set.
  bool AddPhrase(const protocol::Reading& reading, const std::wstring& text, int64_t now = 0,
                 const std::wstring& shortcut = {});
  bool RemovePhrase(const protocol::Reading& reading, const std::wstring& text);

  // For the settings app's user dictionary page.
  struct PhraseRow {
    protocol::Reading reading;
    std::wstring text;
    uint32_t kind = 0;  // 0 manual, 1 learned
    int64_t created = 0;
    std::wstring shortcut;
  };
  struct SelectionRow {
    protocol::Reading reading;
    std::wstring text, prev;
    double weight = 0;  // decayed to now
    int64_t updated = 0;
  };
  std::vector<PhraseRow> ListPhrases();
  // Heaviest first.
  std::vector<SelectionRow> ListSelections(int64_t now = 0);
  // Replaces a phrase (kept manual); false if the new one is invalid.
  bool UpdatePhrase(const protocol::Reading& reading, const std::wstring& text,
                    const protocol::Reading& newReading, const std::wstring& newText,
                    const std::wstring& newShortcut = {});
  // The text of the phrase that `shortcut` types, or "" if none.
  std::wstring ShortcutOwner(const std::wstring& shortcut);
  // A learned phrase kept as if added by hand.
  bool MakeManual(const protocol::Reading& reading, const std::wstring& text);
  // Adds many manual phrases in one transaction; returns how many were new.
  size_t AddPhrases(const std::vector<std::pair<protocol::Reading, std::wstring>>& phrases,
                    int64_t now = 0);
  bool RemoveSelection(const protocol::Reading& reading, const std::wstring& text,
                       const std::wstring& prev);
  // Forgets everything learned: picks, runs of picks and learned phrases (manual ones stay).
  bool ClearLearning();

 private:
  bool Exec(const char* sql);
  void Bump() { ++version_; if (version_ == 0) ++version_; }
  void CheckOutsideChanges();
  static int64_t Now(int64_t now);

  std::mutex mutex_;
  sqlite3* db_ = nullptr;
  uint32_t version_ = 1;
  int64_t dataVersion_ = -1;
};

}  // namespace llaime
