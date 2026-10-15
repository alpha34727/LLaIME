#include <windows.h>

#include <filesystem>

#include "TestLexicon.h"
#include "llaime/ImeSettings.h"
#include "llaime/LexiconBuilder.h"
#include "llaime/LexiconCatalog.h"

using namespace llaime;

namespace {

std::vector<std::wstring> Texts(const std::vector<LexEntry>& entries) {
  std::vector<std::wstring> out;
  for (const auto& e : entries) out.emplace_back(e.text);
  return out;
}

}  // namespace

TEST(LexiconBuilder, RoundTripsThroughLexicon) {
  LexiconBuilder builder;
  builder.Add(Seq({L"ㄌㄧˇ", L"ㄒㄧㄠˇ", L"ㄇㄧㄥˊ"}), L"李小明", -6.0f);
  builder.Add(Seq({L"ㄌㄧˇ", L"ㄒㄧㄠˇ", L"ㄇㄧㄥˊ"}), L"李曉明", -7.0f);
  builder.Add(Seq({L"ㄌㄧˇ", L"ㄒㄧㄠˇ", L"ㄇㄧㄥˊ"}), L"李曉明", -6.5f);  // best kept
  builder.Add(Seq({L"ㄨㄟ", L"ㄐㄧ"}), L"維基", -5.0f);
  EXPECT_EQ(builder.size(), 3u);

  Lexicon lexicon;
  ASSERT_TRUE(lexicon.OpenImage(builder.Build()));
  std::vector<LexEntry> out;
  ASSERT_TRUE(lexicon.Lookup(Seq({L"ㄌㄧˇ", L"ㄒㄧㄠˇ", L"ㄇㄧㄥˊ"}), out));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].text, L"李小明");
  EXPECT_EQ(out[1].text, L"李曉明");
  EXPECT_FLOAT_EQ(out[1].score, -6.5f);

  // Tone-less lookup goes through the toneless index.
  auto toneless = Seq({L"ㄨㄟ", L"ㄐㄧ"});
  for (auto& s : toneless) s = s.withoutTone();
  out.clear();
  ASSERT_TRUE(lexicon.Lookup(toneless, out));
  EXPECT_EQ(out[0].text, L"維基");
}

TEST(Lexicon, LayersMergeKeepingTheBestScore) {
  LexiconBuilder a, b;
  a.Add(Seq({L"ㄕˋ", L"ㄧㄢˋ"}), L"試驗", -5.0f);
  a.Add(Seq({L"ㄕˋ", L"ㄧㄢˋ"}), L"實驗", -6.0f);
  b.Add(Seq({L"ㄕˋ", L"ㄧㄢˋ"}), L"實驗", -4.0f);
  b.Add(Seq({L"ㄕˋ", L"ㄧㄢˋ"}), L"誓言", -7.0f);
  Lexicon lexicon;
  ASSERT_TRUE(lexicon.OpenImage(a.Build()));
  ASSERT_TRUE(lexicon.AddImage(b.Build()));
  EXPECT_EQ(lexicon.layerCount(), 2u);
  std::vector<LexEntry> out;
  ASSERT_TRUE(lexicon.Lookup(Seq({L"ㄕˋ", L"ㄧㄢˋ"}), out));
  EXPECT_EQ(Texts(out), (std::vector<std::wstring>{L"實驗", L"試驗", L"誓言"}));
  EXPECT_FLOAT_EQ(out[0].score, -4.0f);
  EXPECT_TRUE(lexicon.Contains(Seq({L"ㄕˋ", L"ㄧㄢˋ"})));
}

TEST(Lexicon, OpensADirectoryOfLayers) {
  // The test lexicon is data/build/lexicons: the main table and the supplements.
  EXPECT_GE(TestLexicon().layerCount(), 1u);
  std::vector<LexEntry> out;
  EXPECT_TRUE(TestLexicon().LookupNamed(L"_punctuation_<", out));
}

TEST(LexiconCatalog, ListsLayersWithTheirDescriptions) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / L"llaime-catalog-test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  LexiconBuilder builder;
  builder.Add(Seq({L"ㄨㄟ", L"ㄐㄧ"}), L"維基", -5.0f);
  ASSERT_TRUE(builder.Write((dir / L"user-wiki.bin").wstring()));
  ASSERT_TRUE(builder.Write((dir / L"main.bin").wstring()));
  LexiconInfo info;
  info.name = L"維基百科條目";
  info.license = L"CC BY-SA 4.0";
  ASSERT_TRUE(WriteLexiconInfo((dir / L"user-wiki.ini").wstring(), info));
  info.name = L"主詞庫";
  info.required = true;
  info.order = 0;
  ASSERT_TRUE(WriteLexiconInfo((dir / L"main.ini").wstring(), info));

  auto all = ListLexicons(dir.wstring(), true);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].id, L"main");  // required first
  EXPECT_TRUE(all[0].required);
  EXPECT_EQ(all[1].name, L"維基百科條目");
  EXPECT_EQ(all[1].license, L"CC BY-SA 4.0");

  auto enabled = EnabledLexicons(all, {L"main", L"user-wiki"});
  ASSERT_EQ(enabled.size(), 1u);  // a required layer cannot be turned off
  EXPECT_EQ(enabled[0].id, L"main");

  // A user folder never makes a layer required.
  auto user = ListLexicons(dir.wstring(), false);
  EXPECT_FALSE(user[0].required || user[1].required);

  Lexicon lexicon;
  EXPECT_TRUE(lexicon.Open(dir.wstring()));
  EXPECT_EQ(lexicon.layerCount(), 2u);
  lexicon.Clear();
  fs::remove_all(dir);
}

TEST(ReadingGuesser, WholePhraseFromTheLexicon) {
  ReadingGuesser guesser(TestLexicon());
  auto readings = guesser.Guess(L"銀行");
  ASSERT_FALSE(readings.empty());
  EXPECT_EQ(DisplayReading(readings[0]), L"ㄧㄣˊ ㄏㄤˊ");
}

TEST(ReadingGuesser, SplitsUnknownPhrases) {
  ReadingGuesser guesser(TestLexicon());
  // Not a lexicon phrase: 台積電 + 董事長 + 魏哲家.
  auto readings = guesser.Guess(L"台積電董事長");
  ASSERT_FALSE(readings.empty());
  EXPECT_EQ(DisplayReading(readings[0]), L"ㄊㄞˊ ㄐㄧ ㄉㄧㄢˋ ㄉㄨㄥˇ ㄕˋ ㄓㄤˇ");
}

TEST(ReadingGuesser, OffersOtherReadingsOfHeteronyms) {
  ReadingGuesser guesser(TestLexicon());
  // 行 alone: ㄒㄧㄥˊ and ㄏㄤˊ are both listed.
  auto readings = guesser.Guess(L"行");
  std::vector<std::wstring> shown;
  for (const auto& r : readings) shown.push_back(DisplayReading(r));
  EXPECT_NE(std::find(shown.begin(), shown.end(), L"ㄒㄧㄥˊ"), shown.end());
  EXPECT_NE(std::find(shown.begin(), shown.end(), L"ㄏㄤˊ"), shown.end());
}

TEST(ReadingGuesser, UnknownCharacter) {
  ReadingGuesser guesser(TestLexicon());
  EXPECT_TRUE(guesser.Guess(L"abc").empty());
}

TEST(ParseLexiconLine, CommonFormats) {
  auto mcbopomofo = ParseLexiconLine(L"ㄅㄚˋ-ㄅㄚ˙ 爸爸 -5.21");
  ASSERT_TRUE(mcbopomofo);
  EXPECT_EQ(mcbopomofo->text, L"爸爸");
  EXPECT_EQ(DisplayReading(mcbopomofo->reading), L"ㄅㄚˋ ㄅㄚ˙");
  ASSERT_TRUE(mcbopomofo->score);
  EXPECT_FLOAT_EQ(*mcbopomofo->score, -5.21f);

  auto user = ParseLexiconLine(L"維基 ㄨㄟ-ㄐㄧ");
  ASSERT_TRUE(user);
  EXPECT_EQ(user->text, L"維基");
  ASSERT_EQ(user->reading.size(), 2u);
  EXPECT_EQ(user->reading[0].tone(), 1);  // no mark: tone 1
  EXPECT_FALSE(user->score);

  auto csv = ParseLexiconLine(L"爸爸,1234,ㄅㄚˋ ㄅㄚ˙");
  ASSERT_TRUE(csv);
  EXPECT_EQ(csv->text, L"爸爸");
  EXPECT_EQ(csv->reading.size(), 2u);
  EXPECT_FALSE(csv->score);  // a count, not a log10 score

  auto src = ParseLexiconLine(L"爸爸 1234 ㄅㄚˋ ㄅㄚ˙");
  ASSERT_TRUE(src);
  EXPECT_EQ(src->reading.size(), 2u);

  auto word = ParseLexiconLine(L"  魏哲家\r");
  ASSERT_TRUE(word);
  EXPECT_EQ(word->text, L"魏哲家");
  EXPECT_TRUE(word->reading.empty());

  EXPECT_FALSE(ParseLexiconLine(L"# comment"));
  EXPECT_FALSE(ParseLexiconLine(L"   "));
}

TEST(ParseWrittenReading, TonesAndSeparators) {
  auto r = ParseWrittenReading(L"ㄌㄧˇ ㄒㄧㄠˇ-ㄇㄧㄥˊ");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->size(), 3u);
  auto first = ParseWrittenReading(L"ㄐㄧˉ");
  ASSERT_TRUE(first);
  EXPECT_EQ((*first)[0].tone(), 1);
  EXPECT_FALSE(ParseWrittenReading(L"ㄐㄧ x"));
  EXPECT_FALSE(ParseWrittenReading(L""));
}
