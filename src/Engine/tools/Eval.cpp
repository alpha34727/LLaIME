// Candidate-selection evaluation: lexicon-only conversion vs. language-model reranking of the
// lexicon's N best conversions, on a TSV of (id, context, text, reading).
//
//   LLaIME.Eval --lexicon data/build/lexicon.bin --data tests/eval/commonvoice_dev.tsv
//               [--model models/x.gguf] [--device auto|CPU|CUDA0] [--k 16] [--limit N]
//               [--prefix "..."] [--no-context] [--dump out.tsv]
//               [--laya <model dir>] [--threads N]   rank with Laya (ONNX, CPU) instead
//   LLaIME.Eval --laya <model dir> --laya-check expected.tsv
//               checks the C++ tokenizer and input layout against Laya's own
//               (tools/export_laya_tokenizer.py --cases)
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "EvalData.h"
#include "LayaScorer.h"
#include "LlmScorer.h"
#include "llaime/Composer.h"
#include "llaime/Lexicon.h"
#include "llaime/TaiwanUsage.h"

using namespace llaime;
using namespace llaime::eval;

namespace {

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5))];
}

// Characters matching at the same position (readings have one syllable per character).
size_t MatchingChars(const std::wstring& a, const std::wstring& b) {
  size_t n = 0;
  for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) n += a[i] == b[i];
  return n;
}

// Compares BuildInput with the ids and markers Laya's Python build_sequence produced.
int CheckLaya(LayaScorer& laya, const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::string line;
  size_t rows = 0, mismatches = 0;
  auto parseInts = [](const std::string& s) {
    std::vector<int64_t> v;
    std::stringstream ss(s);
    int64_t x;
    while (ss >> x) v.push_back(x);
    return v;
  };
  // Minimal JSON string-array reader for the exported state and options.
  auto parseStrings = [](const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool inString = false;
    for (size_t i = 0; i < s.size(); ++i) {
      const char c = s[i];
      if (!inString) {
        if (c == '"') inString = true, cur.clear();
      } else if (c == '\\' && i + 1 < s.size()) {
        const char e = s[++i];
        cur.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
      } else if (c == '"') {
        inString = false;
        out.push_back(cur);
      } else {
        cur.push_back(c);
      }
    }
    return out;
  };
  while (std::getline(in, line)) {
    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string col;
    while (std::getline(ss, col, '\t')) cols.push_back(col);
    if (cols.size() < 5) continue;
    const auto state = parseStrings(cols[1]);
    const auto options = parseStrings(cols[2]);
    std::vector<int64_t> ids, markers;
    laya.BuildInput(state.empty() ? "" : state[0], options, &ids, &markers);
    ++rows;
    if (ids != parseInts(cols[3]) || markers != parseInts(cols[4])) {
      if (++mismatches <= 5) printf("mismatch: %s\n", cols[0].c_str());
    }
  }
  printf("laya input check: %zu rows, %zu mismatches\n", rows, mismatches);
  return mismatches == 0 && rows > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::map<std::string, std::string> args = {
      {"--lexicon", "data/build/lexicon.bin"},
      {"--data", "tests/eval/commonvoice_dev.tsv"},
      {"--model", ""},
      {"--device", "auto"},
      {"--k", "16"},
      {"--limit", "1000000"},
      {"--prefix", "以下是一段台灣繁體中文的文字：\n"},
      {"--dump", ""},
      {"--laya", ""},
      {"--laya-check", ""},
      {"--threads", "0"},
  };
  bool useContext = true;
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    if (key == "--no-context") {
      useContext = false;
    } else if (args.count(key) && i + 1 < argc) {
      args[key] = argv[++i];
    } else {
      fprintf(stderr, "unknown argument: %s\n", key.c_str());
      return 2;
    }
  }

  LayaScorer laya;
  const bool useLaya = !args["--laya"].empty();
  if (useLaya) {
    LayaScorer::Options options;
    options.modelDir = FromUtf8(args["--laya"]);
    options.threads = std::stoi(args["--threads"]);
    std::string error;
    if (!laya.Load(options, &error)) {
      fprintf(stderr, "laya load failed: %s\n", error.c_str());
      return 1;
    }
    if (!args["--laya-check"].empty()) return CheckLaya(laya, args["--laya-check"]);
  }

  Lexicon lexicon;
  if (!lexicon.Open(FromUtf8(args["--lexicon"]))) {
    fprintf(stderr, "cannot open lexicon %s\n", args["--lexicon"].c_str());
    return 1;
  }
  const auto rows = LoadRows(args["--data"], std::stoul(args["--limit"]));
  const size_t k = std::stoul(args["--k"]);

  LlmScorer scorer;
  const bool useLlm = !args["--model"].empty();
  const bool useModel = useLlm || useLaya;
  if (useLlm) {
    LlmScorer::LoadBackends(ExeDir());
    for (const auto& d : LlmScorer::Devices()) {
      printf("device %-10s %s (%.1f GB)\n", d.name.c_str(), d.description.c_str(),
             d.memoryTotal / 1e9);
    }
    LlmScorer::Options options;
    options.modelPath = args["--model"];
    options.device = args["--device"];
    options.maxCandidates = static_cast<int>(k);
    std::string error;
    auto loadStart = std::chrono::steady_clock::now();
    if (!scorer.Load(options, &error)) {
      fprintf(stderr, "model load failed: %s\n", error.c_str());
      return 1;
    }
    printf("model loaded in %.1f s\n",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - loadStart).count());
  }

  // Per row: candidate texts with lexicon (log10) and LM (ln) scores, and the truth index.
  struct Result {
    std::vector<std::wstring> texts;
    std::vector<double> lex, lm;
    std::vector<int> variants;  // non-Taiwan-standard spellings per candidate
    int truth = -1;
  };
  std::vector<Result> results;
  std::vector<double> latencyMs;
  LlmScorer::Timing timingSum;
  size_t baselineCorrect = 0, baselineChars = 0, totalChars = 0;

  std::ofstream dump;
  if (!args["--dump"].empty()) dump.open(args["--dump"], std::ios::binary);

  for (const Row& row : rows) {
    Composer composer(&lexicon);
    for (Syllable s : row.reading) composer.InsertSyllable(s);
    const std::wstring truth = FromUtf8(row.text);
    const std::wstring baseline = composer.Text();
    baselineCorrect += baseline == truth;
    baselineChars += MatchingChars(baseline, truth);
    totalChars += truth.size();

    Result r;
    std::vector<std::string> candidates;
    for (const Sentence& s : composer.NBest(k)) {
      r.texts.push_back(s.Text());
      r.lex.push_back(s.score);
      r.variants.push_back(CountNonTaiwanVariants(s.Text()));
      candidates.push_back(ToUtf8(s.Text()));
      if (s.Text() == truth) r.truth = static_cast<int>(r.texts.size()) - 1;
    }
    if (useModel) {
      auto start = std::chrono::steady_clock::now();
      if (useLaya) {
        r.lm = laya.Score(useContext ? row.context : "", candidates);
      } else {
        r.lm = scorer.Score(args["--prefix"] + (useContext ? row.context : ""), candidates);
      }
      latencyMs.push_back(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
              .count());
      if (r.lm.size() != r.texts.size()) r.lm.assign(r.texts.size(), 0.0);
      const auto& t = scorer.LastTiming();
      timingSum.context += t.context;
      timingSum.candidates += t.candidates;
      timingSum.softmax += t.softmax;
      timingSum.batchTokens += t.batchTokens;
      timingSum.softmaxRows += t.softmaxRows;
    }
    if (dump) {
      dump << row.id << '\t' << row.text << '\t' << ToUtf8(baseline) << '\t' << r.truth;
      for (size_t i = 0; i < r.texts.size(); ++i) {
        dump << '\t' << ToUtf8(r.texts[i]) << '|' << r.lex[i] << '|'
             << (useModel ? r.lm[i] : 0.0);
      }
      dump << '\n';
    }
    results.push_back(std::move(r));
  }

  const double n = static_cast<double>(results.size());
  size_t oracle = 0;
  for (const auto& r : results) oracle += r.truth >= 0;
  printf("rows %zu  k %zu\n", results.size(), k);
  printf("lexicon      sentence %.1f%%  char %.2f%%\n", 100 * baselineCorrect / n,
         100.0 * baselineChars / totalChars);
  printf("oracle@%zu    sentence %.1f%%\n", k, 100 * oracle / n);

  if (useModel) {
    // Combined score: LM log-prob + weight * lexicon log-prob (converted to natural log)
    // - penalty per non-Taiwan-standard spelling.
    double bestWeight = 0, bestPenalty = 0, bestAcc = -1;
    for (double penalty : {0.0, 2.0, 4.0, 8.0}) {
      printf("penalty %.0f:", penalty);
      for (double weight : {0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0}) {
        size_t correct = 0;
        for (const auto& r : results) {
          size_t pick = 0;
          double best = -1e300;
          for (size_t i = 0; i < r.texts.size(); ++i) {
            double s = r.lm[i] + weight * r.lex[i] * std::log(10.0) - penalty * r.variants[i];
            if (s > best) {
              best = s;
              pick = i;
            }
          }
          correct += static_cast<int>(pick) == r.truth;
        }
        printf("  w%.2f %.1f%%", weight, 100 * correct / n);
        if (correct > bestAcc) {
          bestAcc = static_cast<double>(correct);
          bestWeight = weight;
          bestPenalty = penalty;
        }
      }
      printf("\n");
    }
    printf("best weight %.2f penalty %.0f -> %.1f%%\n", bestWeight, bestPenalty, 100 * bestAcc / n);
    printf("latency ms  p50 %.1f  p95 %.1f  max %.1f\n", Percentile(latencyMs, 0.5),
           Percentile(latencyMs, 0.95), Percentile(latencyMs, 1.0));
    printf("avg ms: context %.1f  candidates %.1f  softmax %.1f  (batch %.0f tokens, %.0f rows)\n",
           timingSum.context / n, timingSum.candidates / n, timingSum.softmax / n,
           timingSum.batchTokens / n, timingSum.softmaxRows / n);
  }
  return 0;
}
