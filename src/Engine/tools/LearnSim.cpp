// Learning simulation: a user types the sentences of one set in order and, whenever the
// conversion is wrong, picks the right words from the candidates (the truth split greedily into
// the longest words the lexicon offers). Each commit teaches the user dictionary exactly as in
// the TIP (CollectLearning -> UserStore -> snapshot), and conversion uses it as the TIP does
// (lexicon merge + LearnedBonus in the combined score).
//
// Reports, per pick:context bonus (nats, see LearnedBonus): sentence accuracy while learning (first pass), when typing the same
// set again (second pass), and on a held-out set never corrected (does learning hurt?).
//
//   LLaIME.LearnSim --lexicon data/build/lexicon.bin --model models/x.gguf
//                   --data tests/eval/commonvoice_dev.tsv --heldout tests/eval/commonvoice_test.tsv
//                   [--limit N] [--bonus 0:0,1:3] [--k 16] [--device auto]
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "EvalData.h"
#include "LlmScorer.h"
#include "UserStore.h"
#include "llaime/Composer.h"
#include "llaime/Learning.h"
#include "llaime/Lexicon.h"
#include "llaime/TaiwanUsage.h"
#include "llaime/UserDictionary.h"

using namespace llaime;
using namespace llaime::eval;
namespace proto = llaime::protocol;

namespace {

constexpr char kPrefix[] = "以下是一段台灣繁體中文的文字：\n";
constexpr double kLexiconWeight = 0.75;  // as in src/Tip/Ranking.cpp
constexpr double kVariantPenalty = 4.0;

class Simulator {
 public:
  Simulator(const Lexicon* lexicon, LlmScorer* scorer, size_t k)
      : lexicon_(lexicon), scorer_(scorer), k_(k) {}

  // Converts like the TIP: the N best with the user dictionary, reranked by the LM.
  void Convert(Composer& composer, const Row& row, const UserDictionary* user,
               std::pair<double, double> bonus) {
    const auto sentences = composer.NBest(k_);
    if (sentences.size() < 2) return;
    std::vector<std::string> texts;
    std::string key = row.context;
    for (const Sentence& s : sentences) {
      texts.push_back(ToUtf8(s.Text()));
      key += '\x1f' + texts.back();
    }
    auto it = cache_.find(key);
    if (it == cache_.end()) {
      it = cache_.emplace(key, scorer_->Score(kPrefix + row.context, texts)).first;
    }
    const auto& lm = it->second;
    if (lm.size() != sentences.size()) return;
    size_t best = 0;
    double bestScore = -1e300;
    for (size_t i = 0; i < sentences.size(); ++i) {
      double score = lm[i] + kLexiconWeight * sentences[i].score * std::log(10.0) -
                     kVariantPenalty * CountNonTaiwanVariants(sentences[i].Text());
      if (user) {
        score += LearnedBonus(*user, composer, sentences[i].nodes, bonus.first, bonus.second);
      }
      if (score > bestScore) {
        bestScore = score;
        best = i;
      }
    }
    composer.SetPreferred(sentences[best]);
  }

  // The user fixes the conversion word by word. False if the truth cannot be reached.
  static bool Correct(Composer& composer, const std::wstring& truth) {
    const size_t n = composer.size();
    if (truth.size() != n) return false;
    for (int round = 0; round < 4 && composer.Text() != truth; ++round) {
      const std::wstring shown = composer.Text();
      std::vector<LexEntry> entries;
      for (size_t i = 0; i < n;) {
        size_t len = std::min(Lexicon::kMaxPhraseLength, n - i);
        for (; len >= 1; --len) {
          const std::wstring word = truth.substr(i, len);
          composer.Lookup(composer.Syllables(i, len), entries);
          if (std::any_of(entries.begin(), entries.end(),
                          [&](const LexEntry& e) { return e.text == word; })) {
            break;
          }
        }
        if (len == 0) return false;  // a character the lexicon does not have for this reading
        if (shown.size() != n || shown.compare(i, len, truth, i, len) != 0) {
          composer.Select({truth.substr(i, len), i, len, 0});
        }
        i += len;
      }
    }
    return composer.Text() == truth;
  }

 private:
  const Lexicon* lexicon_;
  LlmScorer* scorer_;
  size_t k_;
  std::unordered_map<std::string, std::vector<double>> cache_;
};

std::shared_ptr<UserDictionary> Build(const proto::UserDictResponse& snap) {
  auto dict = std::make_shared<UserDictionary>();
  auto syllables = [](const proto::Reading& r) {
    std::vector<Syllable> out;
    for (uint16_t b : r) out.push_back(Syllable(b));
    return out;
  };
  for (const auto& p : snap.phrases) {
    dict->AddPhrase(syllables(p.reading), p.text,
                    p.kind ? UserDictionary::PhraseKind::Learned
                           : UserDictionary::PhraseKind::Manual);
  }
  for (const auto& s : snap.selections) {
    dict->AddSelection(syllables(s.reading), s.text, s.prev, s.weight);
  }
  return dict;
}

}  // namespace

int main(int argc, char** argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::map<std::string, std::string> args = {
      {"--lexicon", "data/build/lexicon.bin"},
      {"--data", "tests/eval/commonvoice_dev.tsv"},
      {"--heldout", "tests/eval/commonvoice_test.tsv"},
      {"--model", ""},
      {"--device", "auto"},
      {"--k", "16"},
      {"--limit", "1000000"},
      {"--bonus", "0:0,1:3"},
  };
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!args.count(argv[i])) {
      fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
    args[argv[i]] = argv[i + 1];
  }
  Lexicon lexicon;
  if (!lexicon.Open(FromUtf8(args["--lexicon"]))) {
    fprintf(stderr, "cannot open lexicon %s\n", args["--lexicon"].c_str());
    return 1;
  }
  LlmScorer::LoadBackends(ExeDir());
  LlmScorer scorer;
  LlmScorer::Options options;
  options.modelPath = args["--model"];
  options.device = args["--device"];
  options.maxCandidates = std::stoi(args["--k"]);
  std::string error;
  if (!scorer.Load(options, &error)) {
    fprintf(stderr, "model load failed: %s\n", error.c_str());
    return 1;
  }
  const size_t limit = std::stoul(args["--limit"]);
  const auto learnRows = LoadRows(args["--data"], limit);
  const auto heldRows = LoadRows(args["--heldout"], limit);
  Simulator sim(&lexicon, &scorer, std::stoul(args["--k"]));

  auto accuracy = [&](const std::vector<Row>& rows, const UserDictionary* user,
                      std::pair<double, double> bonus) {
    size_t correct = 0;
    for (const Row& row : rows) {
      Composer composer(&lexicon);
      composer.SetUserDictionary(user);
      for (Syllable s : row.reading) composer.InsertSyllable(s);
      sim.Convert(composer, row, user, bonus);
      correct += composer.Text() == FromUtf8(row.text);
    }
    return 100.0 * correct / rows.size();
  };

  printf("learn %zu rows, held-out %zu rows\n", learnRows.size(), heldRows.size());
  printf("no learning: learn set %.1f%%  held-out %.1f%%\n", accuracy(learnRows, nullptr, {0, 0}),
         accuracy(heldRows, nullptr, {0, 0}));

  std::stringstream bonuses(args["--bonus"]);
  std::string bonusText;
  while (std::getline(bonuses, bonusText, ',')) {
    // "pick:context" bonuses in nats
    const size_t colon = bonusText.find(':');
    const std::pair<double, double> bonus = {std::stod(bonusText.substr(0, colon)),
                                             std::stod(bonusText.substr(colon + 1))};
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::filesystem::path db =
        std::filesystem::path(tmp) / (L"llaime-learnsim-" + std::to_wstring(GetCurrentProcessId()) +
                                      L".db");
    for (const wchar_t* suffix : {L"", L"-wal", L"-shm"}) {
      std::error_code ec;
      std::filesystem::remove(db.wstring() + suffix, ec);
    }
    UserStore store;
    if (!store.Open(db.wstring(), &error)) {
      fprintf(stderr, "user store: %s\n", error.c_str());
      return 1;
    }
    std::shared_ptr<UserDictionary> user = std::make_shared<UserDictionary>();
    int64_t now = 1'800'000'000;
    size_t correct = 0, unreachable = 0, picks = 0;
    for (const Row& row : learnRows) {
      Composer composer(&lexicon);
      composer.SetUserDictionary(user.get());
      for (Syllable s : row.reading) composer.InsertSyllable(s);
      sim.Convert(composer, row, user.get(), bonus);
      const std::wstring truth = FromUtf8(row.text);
      if (composer.Text() == truth) {
        ++correct;
      } else if (!Simulator::Correct(composer, truth)) {
        ++unreachable;
        continue;
      }
      const LearningEvents events = CollectLearning(composer, composer.size());
      if (events.empty()) continue;
      proto::LearnRequest request;
      for (const auto& s : events.selections) {
        proto::Reading r;
        for (Syllable x : s.reading) r.push_back(x.bits());
        request.selections.push_back({r, s.text, s.prev});
      }
      for (const auto& p : events.phrases) {
        proto::Reading r;
        for (Syllable x : p.reading) r.push_back(x.bits());
        request.phrases.push_back({r, p.text});
      }
      picks += request.selections.size();
      now += 60;
      store.Learn(request, now);
      user = Build(store.Snapshot(0, now));
    }
    const double firstPass = 100.0 * correct / learnRows.size();
    printf("bonus %.1f:%.1f: first pass %.1f%%  second pass %.1f%%  held-out %.1f%%"
           "  (picks %zu, phrases %zu, unreachable %zu)\n",
           bonus.first, bonus.second, firstPass, accuracy(learnRows, user.get(), bonus),
           accuracy(heldRows, user.get(), bonus), picks, user->phraseCount(), unreachable);
    fflush(stdout);
  }
  return 0;
}
