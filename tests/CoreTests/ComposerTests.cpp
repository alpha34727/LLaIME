#include <algorithm>

#include "TestLexicon.h"
#include "llaime/Composer.h"

using namespace llaime;

namespace {

Composer Compose(std::initializer_list<const wchar_t*> syllables) {
  Composer c(&TestLexicon());
  for (Syllable s : Seq(syllables)) c.InsertSyllable(s);
  return c;
}

}  // namespace

TEST(Composer, ConvertsSentence) {
  auto c = Compose({L"ㄨㄛˇ", L"ㄇㄣ˙", L"ㄧˋ", L"ㄑㄧˇ", L"ㄑㄩˋ", L"ㄊㄞˊ", L"ㄅㄟˇ"});
  EXPECT_EQ(c.Text(), L"我們一起去台北");
}

TEST(Composer, CandidatesAtEndPreferLongerPhrases) {
  auto c = Compose({L"ㄧˋ", L"ㄧˋ"});
  auto cands = c.CandidatesAtCursor();
  ASSERT_FALSE(cands.empty());
  EXPECT_EQ(cands[0].text, L"意義");
  EXPECT_EQ(cands[0].length, 2u);
  // Single characters follow the phrases and end at the cursor.
  auto single = std::find_if(cands.begin(), cands.end(),
                             [](const Candidate& x) { return x.length == 1; });
  ASSERT_NE(single, cands.end());
  EXPECT_EQ(single->start, 1u);
}

TEST(Composer, SelectPinsCandidate) {
  auto c = Compose({L"ㄧˋ", L"ㄧˋ"});
  auto cands = c.CandidatesAtCursor();
  auto it = std::find_if(cands.begin(), cands.end(),
                         [](const Candidate& x) { return x.text == L"異議"; });
  ASSERT_NE(it, cands.end());
  c.Select(*it);
  EXPECT_EQ(c.Text(), L"異議");
  // Typing more keeps the pinned phrase.
  for (Syllable s : Seq({L"ㄏㄣˇ", L"ㄉㄨㄛ"})) c.InsertSyllable(s);
  EXPECT_EQ(c.Text().substr(0, 2), L"異議");
}

TEST(Composer, CursorInMiddleUsesSpansStartingThere) {
  auto c = Compose({L"ㄉㄧㄢˋ", L"ㄋㄠˇ", L"ㄓㄨㄛ"});
  c.SetCursor(1);
  auto cands = c.CandidatesAtCursor();
  ASSERT_FALSE(cands.empty());
  for (const auto& x : cands) EXPECT_EQ(x.start, 1u);
}

TEST(Composer, LiteralsSplitPhrases) {
  Composer c(&TestLexicon());
  for (Syllable s : Seq({L"ㄉㄧㄢˋ"})) c.InsertSyllable(s);
  c.InsertLiteral(L"，");
  for (Syllable s : Seq({L"ㄋㄠˇ"})) c.InsertSyllable(s);
  auto text = c.Text();
  EXPECT_EQ(text.size(), 3u);
  EXPECT_EQ(text[1], L'，');
}

TEST(Composer, BackspaceAndDropFront) {
  auto c = Compose({L"ㄉㄧㄢˋ", L"ㄋㄠˇ", L"ㄓㄨㄛ"});
  EXPECT_TRUE(c.DeleteBefore());
  EXPECT_EQ(c.Text(), L"電腦");
  c.DropFront(1);
  EXPECT_EQ(c.size(), 1u);
  EXPECT_EQ(c.cursor(), 1u);
}

TEST(Composer, TextOffset) {
  auto c = Compose({L"ㄉㄧㄢˋ", L"ㄋㄠˇ"});
  EXPECT_EQ(c.TextOffsetOf(0), 0u);
  EXPECT_EQ(c.TextOffsetOf(1), 1u);
  EXPECT_EQ(c.TextOffsetOf(2), 2u);
}

TEST(Composer, NBestStartsWithWalkAndOffersAlternatives) {
  auto c = Compose({L"ㄧˋ", L"ㄧˋ"});
  auto best = c.NBest(8);
  ASSERT_GE(best.size(), 3u);
  EXPECT_EQ(best[0].Text(), c.Text());
  for (size_t i = 1; i < best.size(); ++i) EXPECT_GE(best[i - 1].score, best[i].score);
  bool hasAlternative = std::any_of(best.begin(), best.end(),
                                    [](const Sentence& s) { return s.Text() == L"異議"; });
  EXPECT_TRUE(hasAlternative);
}

TEST(Composer, NBestRespectsPins) {
  auto c = Compose({L"ㄧˋ", L"ㄧˋ"});
  auto cands = c.CandidatesAtCursor();
  auto it = std::find_if(cands.begin(), cands.end(),
                         [](const Candidate& x) { return x.text == L"異議"; });
  ASSERT_NE(it, cands.end());
  c.Select(*it);
  for (const Sentence& s : c.NBest(8)) EXPECT_EQ(s.Text(), L"異議");
}

TEST(Composer, NBestDistinctTexts) {
  auto c = Compose({L"ㄨㄛˇ", L"ㄇㄣ˙", L"ㄧˋ", L"ㄑㄧˇ", L"ㄑㄩˋ", L"ㄊㄞˊ", L"ㄅㄟˇ"});
  auto best = c.NBest(16);
  EXPECT_EQ(best[0].Text(), L"我們一起去台北");
  for (size_t i = 0; i < best.size(); ++i) {
    for (size_t j = i + 1; j < best.size(); ++j) EXPECT_NE(best[i].Text(), best[j].Text());
  }
}
