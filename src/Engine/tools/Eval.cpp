// Candidate-selection evaluation: lexicon-only conversion vs. language-model reranking of the
// lexicon's N best conversions, on a TSV of (id, context, text, reading).
//
//   LLaIME.Eval --lexicon data/build/lexicon.bin --data tests/eval/commonvoice_dev.tsv
//               [--model models/x.gguf] [--device auto|CPU|CUDA0] [--k 16] [--limit N]
//               [--prefix "..."] [--no-context] [--dump out.tsv]
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

#include "LlmScorer.h"
#include "llaime/Composer.h"
#include "llaime/Lexicon.h"

using namespace llaime;

namespace {

std::string ToUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

std::wstring FromUtf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

struct Row {
  std::string id, context, text;
  std::vector<Syllable> reading;
};

std::vector<Row> LoadRows(const std::string& path, size_t limit) {
  std::vector<Row> rows;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line) && rows.size() < limit) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> cols;
    std::stringstream ss(line);
    std::string col;
    while (std::getline(ss, col, '\t')) cols.push_back(col);
    if (cols.size() < 4) continue;
    Row row{cols[0], cols[1], cols[2], {}};
    std::wstringstream syllables(FromUtf8(cols[3]));
    std::wstring s;
    bool ok = true;
    while (syllables >> s) {
      auto parsed = Syllable::Parse(s);
      if (!parsed) {
        ok = false;
        break;
      }
      row.reading.push_back(*parsed);
    }
    if (ok) rows.push_back(std::move(row));
  }
  return rows;
}

std::string ExeDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p(path);
  return ToUtf8(p.substr(0, p.find_last_of(L"\\/")));
}

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

  Lexicon lexicon;
  if (!lexicon.Open(FromUtf8(args["--lexicon"]))) {
    fprintf(stderr, "cannot open lexicon %s\n", args["--lexicon"].c_str());
    return 1;
  }
  const auto rows = LoadRows(args["--data"], std::stoul(args["--limit"]));
  const size_t k = std::stoul(args["--k"]);

  LlmScorer scorer;
  const bool useModel = !args["--model"].empty();
  if (useModel) {
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
    int truth = -1;
  };
  std::vector<Result> results;
  std::vector<double> latencyMs;
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
      candidates.push_back(ToUtf8(s.Text()));
      if (s.Text() == truth) r.truth = static_cast<int>(r.texts.size()) - 1;
    }
    if (useModel) {
      const std::string context = args["--prefix"] + (useContext ? row.context : "");
      auto start = std::chrono::steady_clock::now();
      r.lm = scorer.Score(context, candidates);
      latencyMs.push_back(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
              .count());
      if (r.lm.size() != r.texts.size()) r.lm.assign(r.texts.size(), 0.0);
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
    // Combined score: LM log-prob + weight * lexicon log-prob (converted to natural log).
    double bestWeight = 0, bestAcc = -1;
    for (double weight : {0.0, 0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0}) {
      size_t correct = 0;
      for (const auto& r : results) {
        size_t pick = 0;
        double best = -1e300;
        for (size_t i = 0; i < r.texts.size(); ++i) {
          double s = r.lm[i] + weight * r.lex[i] * std::log(10.0);
          if (s > best) {
            best = s;
            pick = i;
          }
        }
        correct += static_cast<int>(pick) == r.truth;
      }
      printf("lm+%.2f*lex  sentence %.1f%%\n", weight, 100 * correct / n);
      if (correct > bestAcc) {
        bestAcc = static_cast<double>(correct);
        bestWeight = weight;
      }
    }
    printf("best weight %.2f -> %.1f%%\n", bestWeight, 100 * bestAcc / n);
    printf("latency ms  p50 %.1f  p95 %.1f  max %.1f\n", Percentile(latencyMs, 0.5),
           Percentile(latencyMs, 0.95), Percentile(latencyMs, 1.0));
  }
  return 0;
}
