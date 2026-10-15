#include <algorithm>

#include "TestLexicon.h"
#include "llaime/Composer.h"
#include "llaime/Learning.h"
#include "llaime/UserDictionary.h"

using namespace llaime;

namespace {

Composer Compose(const UserDictionary* user, std::initializer_list<const wchar_t*> syllables) {
  Composer c(&TestLexicon());
  c.SetUserDictionary(user);
  for (Syllable s : Seq(syllables)) c.InsertSyllable(s);
  return c;
}

const Candidate* Find(const std::vector<Candidate>& cands, const std::wstring& text) {
  auto it = std::find_if(cands.begin(), cands.end(),
                         [&](const Candidate& c) { return c.text == text; });
  return it == cands.end() ? nullptr : &*it;
}

}  // namespace

TEST(Reading, FormatAndParseRoundTrip) {
  auto seq = Seq({L"ㄌㄧˇ", L"ㄒㄧㄠˇ", L"ㄇㄧㄥˊ", L"ㄊㄧㄢ"});
  const std::wstring text = FormatReading(seq);
  EXPECT_EQ(text, L"ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ-ㄊㄧㄢˉ");
  auto parsed = ParseReading(text);
  ASSERT_TRUE(parsed);
  EXPECT_TRUE(std::equal(parsed->begin(), parsed->end(), seq.begin(), seq.end()));
  // Without a mark the syllable has no tone and matches any.
  auto toneless = ParseReading(L"ㄊㄧㄢ");
  ASSERT_TRUE(toneless);
  EXPECT_FALSE((*toneless)[0].hasTone());
  EXPECT_FALSE(ParseReading(L""));
  EXPECT_FALSE(ParseReading(L"abc"));
}

TEST(UserDictionary, ManualPhraseConvertsAndLeadsCandidates) {
  UserDictionary user;
  user.AddPhrase(Seq({L"ㄅㄛˊ", L"ㄩㄣˊ"}), L"柏昀", UserDictionary::PhraseKind::Manual);
  auto plain = Compose(nullptr, {L"ㄅㄛˊ", L"ㄩㄣˊ"});
  EXPECT_NE(plain.Text(), L"柏昀");
  auto c = Compose(&user, {L"ㄅㄛˊ", L"ㄩㄣˊ"});
  EXPECT_EQ(c.Text(), L"柏昀");
  auto cands = c.CandidatesAtCursor();
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands[0].text, L"柏昀");
  // Inside a longer sentence too.
  auto s = Compose(&user, {L"ㄨㄛˇ", L"ㄕˋ", L"ㄅㄛˊ", L"ㄩㄣˊ"});
  EXPECT_NE(s.Text().find(L"柏昀"), std::wstring::npos);
}

TEST(UserDictionary, PickedWordComesFirstWithoutResegmenting) {
  auto plain = Compose(nullptr, {L"ㄧˋ", L"ㄧˋ"});
  ASSERT_EQ(plain.Text(), L"意義");
  UserDictionary user;
  user.AddSelection(Seq({L"ㄧˋ", L"ㄧˋ"}), L"異議", L"", 1.0);
  auto c = Compose(&user, {L"ㄧˋ", L"ㄧˋ"});
  EXPECT_EQ(c.Text(), L"異議");
  EXPECT_EQ(c.CandidatesAtCursor()[0].text, L"異議");

  // A picked single character does not break up the phrases around it.
  UserDictionary single;
  single.AddSelection(Seq({L"ㄧˋ"}), L"議", L"", 1.0);
  auto w = Compose(&single, {L"ㄧˋ", L"ㄧˋ"});
  EXPECT_EQ(w.Text(), L"意義");
}

TEST(UserDictionary, WeakerPicksRankBelowStronger) {
  UserDictionary user;
  user.AddSelection(Seq({L"ㄧˋ", L"ㄧˋ"}), L"異議", L"", 0.3);
  user.AddSelection(Seq({L"ㄧˋ", L"ㄧˋ"}), L"毅議", L"", 1.0);  // not offered: ignored
  user.AddSelection(Seq({L"ㄧˋ", L"ㄧˋ"}), L"意義", L"", 0.9);
  auto c = Compose(&user, {L"ㄧˋ", L"ㄧˋ"});
  auto cands = c.CandidatesAtCursor();
  EXPECT_EQ(cands[0].text, L"意義");
  EXPECT_EQ(cands[1].text, L"異議");
  EXPECT_FALSE(Find(cands, L"毅議"));
}

TEST(Learning, CollectsPicksAndRunsOfPicks) {
  auto c = Compose(nullptr, {L"ㄨㄛˇ", L"ㄕˋ", L"ㄅㄛˊ", L"ㄩㄣˊ"});
  // Pick 柏 and 昀 one by one.
  c.SetCursor(2);
  auto first = c.CandidatesAtCursor();
  const Candidate* bo = Find(first, L"柏");
  ASSERT_TRUE(bo);
  c.Select(*bo);
  c.SetCursor(4);
  auto second = c.CandidatesAtCursor();
  const Candidate* yun = Find(second, L"昀");
  ASSERT_TRUE(yun);
  c.Select(*yun);
  ASSERT_EQ(c.Text().substr(2), L"柏昀");

  LearningEvents events = CollectLearning(c, c.size());
  ASSERT_EQ(events.selections.size(), 2u);
  EXPECT_EQ(events.selections[0].text, L"柏");
  EXPECT_EQ(events.selections[1].text, L"昀");
  EXPECT_EQ(events.selections[1].prev, L"柏");
  ASSERT_EQ(events.phrases.size(), 1u);
  EXPECT_EQ(events.phrases[0].text, L"柏昀");
  EXPECT_EQ(FormatReading(events.phrases[0].reading), L"ㄅㄛˊ-ㄩㄣˊ");

  // Only the committed part counts.
  EXPECT_TRUE(CollectLearning(c, 2).empty());
}

TEST(Learning, NothingWithoutPicks) {
  auto c = Compose(nullptr, {L"ㄨㄛˇ", L"ㄇㄣ˙", L"ㄧˋ", L"ㄑㄧˇ"});
  EXPECT_TRUE(CollectLearning(c, c.size()).empty());
}

TEST(Learning, BonusForPickedWordsAndContext) {
  UserDictionary user;
  user.AddSelection(Seq({L"ㄧˋ", L"ㄧˋ"}), L"異議", L"", 0.5);
  user.AddSelection(Seq({L"ㄧˋ", L"ㄧˋ"}), L"異議", L"提出", 2.0);
  auto c = Compose(&user, {L"ㄊㄧˊ", L"ㄔㄨ", L"ㄧˋ", L"ㄧˋ"});
  const auto& nodes = c.Walk();
  ASSERT_EQ(nodes.size(), 2u);
  ASSERT_EQ(nodes[0].text, L"提出");
  ASSERT_EQ(nodes[1].text, L"異議");
  EXPECT_DOUBLE_EQ(LearnedBonus(user, c, nodes), 0.5 * kPickBonus + kPickContextBonus);
  std::vector<Node> other = nodes;
  other[1].text = L"意義";
  EXPECT_DOUBLE_EQ(LearnedBonus(user, c, other), 0.0);
}
