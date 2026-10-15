#pragma once
#include <cstdint>
#include <mutex>
#include <string>

#include "llaime/Protocol.h"

struct sqlite3;

namespace llaime {

// The user's phrases and what they taught by picking candidates, in
// %LOCALAPPDATA%\LLaIME\user.db (SQLite, WAL). The engine is its only writer among the IME's
// processes; the settings app may edit it too, which shows up as a new version.
//
//   phrases     (reading, text, kind 0 manual / 1 learned, created)
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
  bool AddPhrase(const protocol::Reading& reading, const std::wstring& text, int64_t now = 0);
  bool RemovePhrase(const protocol::Reading& reading, const std::wstring& text);

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
