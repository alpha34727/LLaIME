#include <gtest/gtest.h>
#include <windows.h>

#include <filesystem>
#include <memory>

#include "UserStore.h"
#include "llaime/UserDictionary.h"

using namespace llaime;
namespace proto = llaime::protocol;

namespace {

constexpr int64_t kDay = 86400;
constexpr int64_t kT0 = 1'800'000'000;

proto::Reading R(const wchar_t* text) {
  proto::Reading out;
  const auto syllables = ParseReading(text);
  for (Syllable s : *syllables) out.push_back(s.bits());
  return out;
}

class UserStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    path_ = std::filesystem::path(dir) /
            (L"llaime-test-" + std::to_wstring(GetCurrentProcessId()) + L".db");
    Remove();
    ASSERT_TRUE(store_->Open(path_.wstring()));
  }
  void TearDown() override {
    store_.reset();
    Remove();
  }
  void Remove() {
    for (const wchar_t* suffix : {L"", L"-wal", L"-shm"}) {
      std::error_code ec;
      std::filesystem::remove(path_.wstring() + suffix, ec);
    }
  }
  void Pick(const wchar_t* reading, const wchar_t* text, const wchar_t* prev, int64_t now) {
    proto::LearnRequest req;
    req.selections.push_back({R(reading), text, prev});
    ASSERT_TRUE(store_->Learn(req, now));
  }
  double Weight(const proto::UserDictResponse& snap, const std::wstring& text,
                const std::wstring& prev) {
    for (const auto& s : snap.selections) {
      if (s.text == text && s.prev == prev) return s.weight;
    }
    return 0;
  }

  std::filesystem::path path_;
  std::unique_ptr<UserStore> store_ = std::make_unique<UserStore>();
};

}  // namespace

TEST_F(UserStoreTest, PicksAddUpAndDecay) {
  Pick(L"ㄧˋ-ㄧˋ", L"異議", L"提出", kT0);
  auto snap = store_->Snapshot(0, kT0);
  EXPECT_TRUE(snap.changed);
  EXPECT_DOUBLE_EQ(Weight(snap, L"異議", L""), 1.0);
  EXPECT_DOUBLE_EQ(Weight(snap, L"異議", L"提出"), 1.0);
  Pick(L"ㄧˋ-ㄧˋ", L"異議", L"", kT0);
  snap = store_->Snapshot(0, kT0);
  EXPECT_DOUBLE_EQ(Weight(snap, L"異議", L""), 2.0);
  // Half after the half-life.
  snap = store_->Snapshot(0, kT0 + static_cast<int64_t>(UserStore::kHalfLifeDays) * kDay);
  EXPECT_NEAR(Weight(snap, L"異議", L""), 1.0, 1e-9);
}

TEST_F(UserStoreTest, NewPickHalvesRivals) {
  Pick(L"ㄧˋ-ㄧˋ", L"異議", L"", kT0);
  Pick(L"ㄧˋ-ㄧˋ", L"意義", L"", kT0);
  auto snap = store_->Snapshot(0, kT0);
  EXPECT_DOUBLE_EQ(Weight(snap, L"異議", L""), 0.5);
  EXPECT_DOUBLE_EQ(Weight(snap, L"意義", L""), 1.0);
  // Faded picks are not sent.
  snap = store_->Snapshot(0, kT0 + 200 * kDay);
  EXPECT_EQ(snap.selections.size(), 0u);
}

TEST_F(UserStoreTest, RunsBecomePhrasesWhenRepeated) {
  proto::LearnRequest req;
  req.phrases.push_back({R(L"ㄅㄛˊ-ㄩㄣˊ"), L"柏昀"});
  ASSERT_TRUE(store_->Learn(req, kT0));
  EXPECT_TRUE(store_->Snapshot(0, kT0).phrases.empty());
  ASSERT_TRUE(store_->Learn(req, kT0 + 1));
  auto snap = store_->Snapshot(0, kT0);
  ASSERT_EQ(snap.phrases.size(), 1u);
  EXPECT_EQ(snap.phrases[0].text, L"柏昀");
  EXPECT_EQ(snap.phrases[0].kind, 1u);
  EXPECT_EQ(snap.phrases[0].reading, R(L"ㄅㄛˊ-ㄩㄣˊ"));
}

TEST_F(UserStoreTest, ManualPhrasesAndVersions) {
  const uint32_t v0 = store_->Version();
  EXPECT_NE(v0, 0u);
  EXPECT_FALSE(store_->Snapshot(v0).changed);
  ASSERT_TRUE(store_->AddPhrase(R(L"ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ"), L"李小明", kT0));
  const uint32_t v1 = store_->Version();
  EXPECT_NE(v1, v0);
  auto snap = store_->Snapshot(v0);
  ASSERT_EQ(snap.phrases.size(), 1u);
  EXPECT_EQ(snap.phrases[0].kind, 0u);
  // Invalid input is refused.
  EXPECT_FALSE(store_->AddPhrase({}, L"空"));
  EXPECT_FALSE(store_->AddPhrase(R(L"ㄎㄨㄥ"), L""));
  EXPECT_FALSE(store_->AddPhrase(R(L"ㄎㄨㄥ"), L"a\nb"));
  ASSERT_TRUE(store_->RemovePhrase(R(L"ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ"), L"李小明"));
  EXPECT_TRUE(store_->Snapshot(0).phrases.empty());
}

TEST_F(UserStoreTest, PausedLearningKeepsNothing) {
  ASSERT_TRUE(store_->SetLearningEnabled(false));
  EXPECT_FALSE(store_->LearningEnabled());
  Pick(L"ㄧˋ-ㄧˋ", L"異議", L"", kT0);
  auto snap = store_->Snapshot(0, kT0);
  EXPECT_FALSE(snap.learningEnabled);
  EXPECT_TRUE(snap.selections.empty());
  // Phrases added by hand still work.
  EXPECT_TRUE(store_->AddPhrase(R(L"ㄎㄨㄥ"), L"空"));
  ASSERT_TRUE(store_->SetLearningEnabled(true));
  EXPECT_TRUE(store_->LearningEnabled());
}

TEST_F(UserStoreTest, EditsFromAnotherProcessChangeTheVersion) {
  const uint32_t v0 = store_->Version();
  UserStore other;  // like the settings app editing user.db
  ASSERT_TRUE(other.Open(path_.wstring()));
  ASSERT_TRUE(other.AddPhrase(R(L"ㄎㄨㄥ"), L"空"));
  EXPECT_NE(store_->Version(), v0);
  EXPECT_EQ(store_->Snapshot(0).phrases.size(), 1u);
}

TEST_F(UserStoreTest, ManagesPhrasesForTheSettingsApp) {
  ASSERT_TRUE(store_->AddPhrase(R(L"ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ"), L"李小明", kT0));
  const uint32_t before = store_->Version();
  ASSERT_TRUE(store_->UpdatePhrase(R(L"ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ"), L"李小明",
                                   R(L"ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ"), L"李曉明"));
  EXPECT_NE(store_->Version(), before);
  auto rows = store_->ListPhrases();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].text, L"李曉明");
  EXPECT_EQ(rows[0].created, kT0);  // editing keeps the date it was added
  EXPECT_FALSE(store_->UpdatePhrase(R(L"ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ"), L"李曉明",
                                    R(L"ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ"), L"bad\ttext"));

  std::vector<std::pair<proto::Reading, std::wstring>> batch = {
      {R(L"ㄨㄟˊ-ㄐㄧ"), L"維基"}, {R(L"ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ"), L"李曉明"}, {{}, L"壞"}};
  EXPECT_EQ(store_->AddPhrases(batch, kT0), 2u);  // the invalid reading is skipped
  EXPECT_EQ(store_->ListPhrases().size(), 2u);
}

TEST_F(UserStoreTest, ClearLearningKeepsManualPhrases) {
  ASSERT_TRUE(store_->AddPhrase(R(L"ㄨㄟˊ-ㄐㄧ"), L"維基", kT0));
  Pick(L"ㄕˋ-ㄧㄢˋ", L"實驗", L"做", kT0);
  proto::LearnRequest runs;
  runs.phrases.push_back({R(L"ㄐㄧ-ㄉㄢˋ"), L"機蛋"});
  ASSERT_TRUE(store_->Learn(runs, kT0));
  ASSERT_TRUE(store_->Learn(runs, kT0));  // promoted to a learned phrase
  ASSERT_EQ(store_->ListPhrases().size(), 2u);
  auto picks = store_->ListSelections(kT0);
  ASSERT_EQ(picks.size(), 2u);  // overall and after 做
  EXPECT_DOUBLE_EQ(picks[0].weight, 1.0);

  ASSERT_TRUE(store_->RemoveSelection(R(L"ㄕˋ-ㄧㄢˋ"), L"實驗", L"做"));
  EXPECT_EQ(store_->ListSelections(kT0).size(), 1u);

  ASSERT_TRUE(store_->ClearLearning());
  EXPECT_TRUE(store_->ListSelections(kT0).empty());
  auto phrases = store_->ListPhrases();
  ASSERT_EQ(phrases.size(), 1u);
  EXPECT_EQ(phrases[0].text, L"維基");
}

TEST_F(UserStoreTest, LearnedPhraseMadeManual) {
  proto::LearnRequest runs;
  runs.phrases.push_back({R(L"ㄐㄧ-ㄉㄢˋ"), L"機蛋"});
  ASSERT_TRUE(store_->Learn(runs, kT0));
  ASSERT_TRUE(store_->Learn(runs, kT0));
  ASSERT_EQ(store_->ListPhrases().at(0).kind, 1u);
  ASSERT_TRUE(store_->MakeManual(R(L"ㄐㄧ-ㄉㄢˋ"), L"機蛋"));
  EXPECT_EQ(store_->ListPhrases().at(0).kind, 0u);
  ASSERT_TRUE(store_->ClearLearning());
  EXPECT_EQ(store_->ListPhrases().size(), 1u);
}

TEST_F(UserStoreTest, PhrasesTypedByShortcut) {
  // A symbol with no reading, typed by its shortcut alone.
  ASSERT_TRUE(store_->AddPhrase({}, L"「」", kT0, L"mk"));
  ASSERT_TRUE(store_->AddPhrase(R(L"ㄨㄟˋ-ㄓㄜˊ-ㄐㄧㄚˉ"), L"魏哲家", kT0, L"wzj"));
  EXPECT_FALSE(store_->AddPhrase({}, L"沒有注音也沒有快捷鍵", kT0));
  EXPECT_FALSE(store_->AddPhrase({}, L"『』", kT0, L"mk"));  // taken
  EXPECT_FALSE(store_->AddPhrase({}, L"『』", kT0, L"M-K"));
  EXPECT_EQ(store_->ShortcutOwner(L"mk"), L"「」");

  auto snap = store_->Snapshot(0, kT0);
  ASSERT_EQ(snap.shortcuts.size(), 2u);
  ASSERT_EQ(snap.phrases.size(), 1u);  // only 魏哲家 converts from its reading
  EXPECT_EQ(snap.phrases[0].text, L"魏哲家");

  auto rows = store_->ListPhrases();
  ASSERT_EQ(rows.size(), 2u);
  // Changing the shortcut, and removing a phrase that has no reading.
  ASSERT_TRUE(store_->UpdatePhrase({}, L"「」", {}, L"「」", L"qq"));
  EXPECT_EQ(store_->ShortcutOwner(L"mk"), L"");
  EXPECT_EQ(store_->ShortcutOwner(L"qq"), L"「」");
  ASSERT_TRUE(store_->RemovePhrase({}, L"「」"));
  EXPECT_EQ(store_->ListPhrases().size(), 1u);

  // The snapshot round-trips through the protocol, and a reply without shortcuts (an older
  // engine) still decodes.
  auto bytes = proto::Encode(store_->Snapshot(0, kT0));
  proto::Reader in(bytes.data(), bytes.size());
  proto::UserDictResponse decoded;
  ASSERT_EQ(in.Header(), proto::MessageType::UserDictResponse);
  ASSERT_TRUE(proto::Decode(in, &decoded));
  ASSERT_EQ(decoded.shortcuts.size(), 1u);
  EXPECT_EQ(decoded.shortcuts[0].keys, L"wzj");
  bytes.resize(bytes.size() - (4 + 4 + 3 * 2 + 4 + 3 * 2));  // drop the shortcut list
  proto::Reader old(bytes.data(), bytes.size());
  ASSERT_EQ(old.Header(), proto::MessageType::UserDictResponse);
  ASSERT_TRUE(proto::Decode(old, &decoded));
  EXPECT_TRUE(decoded.shortcuts.empty());
}
