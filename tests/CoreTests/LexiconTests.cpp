#include "TestLexicon.h"

using namespace llaime;

TEST(Lexicon, ExactLookupBestFirst) {
  std::vector<LexEntry> out;
  ASSERT_TRUE(TestLexicon().Lookup(Seq({L"ㄧˋ", L"ㄧˋ"}), out));
  ASSERT_GE(out.size(), 3u);
  EXPECT_EQ(out[0].text, L"意義");
  for (size_t i = 1; i < out.size(); ++i) EXPECT_GE(out[i - 1].score, out[i].score);
}

TEST(Lexicon, MissingKey) {
  std::vector<LexEntry> out;
  EXPECT_FALSE(TestLexicon().Lookup(Seq({L"ㄅㄧㄤˋ", L"ㄅㄧㄤˋ", L"ㄅㄧㄤˋ"}), out));
  EXPECT_TRUE(out.empty());
}

TEST(Lexicon, TonelessLookup) {
  auto seq = Seq({L"ㄉㄧㄢˋ", L"ㄋㄠˇ"});
  seq[0] = seq[0].withoutTone();
  seq[1] = seq[1].withoutTone();
  std::vector<LexEntry> out;
  ASSERT_TRUE(TestLexicon().Lookup(seq, out));
  EXPECT_EQ(out[0].text, L"電腦");
}

TEST(Lexicon, PartialTonesMustMatch) {
  // ㄉㄧㄢˋ fixed, second syllable tone-less.
  auto seq = Seq({L"ㄉㄧㄢˋ", L"ㄋㄠˇ"});
  seq[1] = seq[1].withoutTone();
  std::vector<LexEntry> out;
  ASSERT_TRUE(TestLexicon().Lookup(seq, out));
  EXPECT_EQ(out[0].text, L"電腦");
  seq[0] = *Syllable::Parse(L"ㄉㄧㄢ");  // tone 1 explicitly: no 電腦
  out.clear();
  TestLexicon().Lookup(seq, out);
  for (const auto& e : out) EXPECT_NE(e.text, L"電腦");
}

TEST(Lexicon, NamedTable) {
  std::vector<LexEntry> out;
  ASSERT_TRUE(TestLexicon().LookupNamed(L"_punctuation_<", out));
  EXPECT_EQ(out[0].text, L"，");
}
