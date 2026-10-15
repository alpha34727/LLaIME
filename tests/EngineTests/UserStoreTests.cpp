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
