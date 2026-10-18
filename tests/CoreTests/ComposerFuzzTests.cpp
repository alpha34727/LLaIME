// Random editing sessions against the composer, the way the TIP drives it (typing, cursor
// moves, candidate picks, model reranking, committing the front of a long buffer, learning).
// Any exception escaping here would escape a TSF callback in the host app and end it.
#include <random>
#include <sstream>

#include "TestLexicon.h"
#include "llaime/Composer.h"
#include "llaime/Learning.h"

using namespace llaime;

namespace {

const wchar_t* const kSyllables[] = {
    L"ㄨㄛˇ", L"ㄇㄣ˙", L"ㄧˋ", L"ㄑㄧˇ", L"ㄑㄩˋ", L"ㄊㄞˊ", L"ㄅㄟˇ", L"ㄗㄠˇ", L"ㄢ",
    L"ㄐㄧㄣ", L"ㄊㄧㄢ", L"ㄑㄧˋ", L"ㄑㄧㄥˊ", L"ㄒㄧㄤˇ", L"ㄔ", L"ㄉㄢˋ", L"ㄅㄧˇ", L"ㄉㄜˊ",
    L"ㄓㄨˇ", L"ㄔㄥˊ", L"ㄒㄩˋ", L"ㄐㄧˋ", L"ㄕˊ", L"ㄐㄧㄢ", L"ㄩˇ", L"ㄐㄧㄝˊ", L"ㄕㄨˋ",
    L"ㄉㄞˋ", L"ㄇㄚˇ", L"ㄏㄨㄟˋ", L"ㄈㄢ", L"ㄧˋ", L"ㄔㄥˊ", L"ㄎㄢˋ", L"ㄉㄨㄥˇ", L"ㄉㄜ˙",
    L"ㄧ", L"ㄙ", L"ㄖㄨˋ", L"ㄦˊ", L"ㄅㄧㄤˋ", L"ㄈㄥ"};
const wchar_t* const kLiterals[] = {L"，", L"。", L"]", L"[", L"「", L"」", L"？", L"a", L"1"};

}  // namespace

TEST(ComposerFuzz, RandomSessionsNeverThrow) {
  std::mt19937 rng(20261018);
  UserDictionary user;
  user.AddPhrase(Seq({L"ㄗㄠˇ", L"ㄢ"}), L"早安", UserDictionary::PhraseKind::Manual);
  user.AddSelection(Seq({L"ㄧˋ"}), L"意", L"", 1.0);
  for (int session = 0; session < 300; ++session) {
    Composer c(&TestLexicon());
    if (session % 2) c.SetUserDictionary(&user);
    std::wostringstream log;
    try {
      const int ops = 20 + static_cast<int>(rng() % 200);
      // Some sessions are mostly punctuation, typed in long runs.
      const unsigned literalShare = rng() % 80;
      for (int op = 0; op < ops; ++op) {
        unsigned kind = rng() % 100;
        if (rng() % 100 < literalShare) kind = 56;
        if (kind < 55) {
          Syllable s = *Syllable::Parse(kSyllables[rng() % std::size(kSyllables)]);
          if (rng() % 5 == 0) s = s.withoutTone();
          log << L"ins ";
          c.InsertSyllable(s);
        } else if (kind < 62) {
          log << L"lit ";
          c.InsertLiteral(kLiterals[rng() % std::size(kLiterals)]);
        } else if (kind < 70) {
          const size_t pos = c.size() ? rng() % (c.size() + 1) : 0;
          log << L"cur" << pos << L' ';
          c.SetCursor(pos);
        } else if (kind < 75) {
          log << L"bs ";
          c.DeleteBefore();
        } else if (kind < 78) {
          log << L"del ";
          c.DeleteAfter();
        } else if (kind < 86) {
          auto cands = c.CandidatesAtCursor();
          if (!cands.empty()) {
            log << L"sel ";
            c.Select(cands[rng() % cands.size()]);
          }
        } else if (kind < 92) {
          auto nbest = c.NBest(16);
          if (!nbest.empty()) {
            log << L"pref ";
            c.SetPreferred(nbest[rng() % nbest.size()]);
          }
          LearnedBonus(user, c, c.Walk());
        } else {
          // CommitOverflow: past 40 elements with the cursor at the end, commit the front.
          if (c.size() > 40 && c.cursor() == c.size()) {
            size_t drop = 0;
            for (const Node& node : c.Walk()) {
              if (c.size() - drop <= 20) break;
              drop += node.length;
            }
            log << L"drop" << drop << L' ';
            CollectLearning(c, drop);
            c.DropFront(drop);
          } else {
            log << L"learn ";
            CollectLearning(c, c.size());
          }
        }
        // What every keystroke reads back.
        c.Text();
        c.TextOffsetOf(c.cursor());
        for (const Node& node : c.Walk()) {
          ASSERT_LE(node.start + node.length, c.size()) << log.str();
          c.IsReadingSpan(node.start, node.length);
        }
      }
    } catch (const std::exception& e) {
      FAIL() << "session " << session << ": " << e.what() << "\nops: "
             << std::string(log.str().begin(), log.str().end());
    }
  }
}
