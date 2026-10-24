// Candidate-selection evaluation: lexicon-only conversion vs. language-model reranking of the
// lexicon's N best conversions, on a TSV of (id, context, text, reading[, following]).
//
//   LLaIME.Eval --lexicon data/build/lexicons --data tests/eval/commonvoice_dev.tsv
//               [--model models/x.gguf] [--device auto|CPU|CUDA0] [--k 16] [--limit N]
//               [--prefix "..."] [--no-context] [--dump out.tsv]
//               [--batch-layout auto|trie|padded] [--per-span N]
//               [--llm-decode B]  also let the model write the text itself (beam B)
//               [--decode-lexicon-weight W]  ... adding W x the lexicon's score of the text
//               [--following-chars N]  the LLM also sees up to N characters after the
//                                       segment (0, the default: none; the IME reads 32)
//               [--laya <model dir>] [--threads N] [--laya-device CPU|CUDA0]
//                                       rank with Laya (ONNX Runtime) instead
//   LLaIME.Eval --laya <model dir> --laya-check expected.tsv
//               checks the C++ tokenizer and input layout against Laya's own
//               (tools/export_laya_tokenizer.py --cases)
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <span>
#include <unordered_map>
#include <unordered_set>
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

// LLM-led conversion (experiment, --llm-decode B): instead of reranking the lexicon's N best
// sentences, the model writes the text itself, one vocabulary token at a time, limited to
// tokens whose every character has the next syllables as a reading (from the lexicon). Beam
// search over character positions: hypotheses covering the same characters compete, B kept per
// position, one model call per position. Where a character has no token of its own (rare
// characters split into byte pieces), the lexicon's best characters for that syllable fill in,
// scored with their lexicon probability. The pick is the best full text, with the same
// non-Taiwan-spelling penalty as the reranker.
// log10 probability of `text` as the lexicon's best segmentation into entries read as
// `reading` (one syllable per character); -infinity when it cannot be segmented.
double LexiconScore(const Lexicon& lexicon, const std::vector<Syllable>& reading,
                    const std::wstring& text) {
  const size_t n = text.size();
  std::vector<double> best(n + 1, -std::numeric_limits<double>::infinity());
  best[0] = 0;
  std::vector<LexEntry> entries;
  for (size_t i = 0; i < n; ++i) {
    if (best[i] == -std::numeric_limits<double>::infinity()) continue;
    for (size_t len = 1; len <= Lexicon::kMaxPhraseLength && i + len <= n; ++len) {
      entries.clear();
      if (!lexicon.Lookup(std::span<const Syllable>(&reading[i], len), entries)) continue;
      for (const LexEntry& e : entries) {
        if (e.text == std::wstring_view(text).substr(i, len)) {
          best[i + len] = std::max(best[i + len], best[i] + e.score);
          break;
        }
      }
    }
  }
  return best[n];
}

struct DecodeResult {
  std::wstring text;
  size_t fills = 0;  // characters the lexicon supplied
};

DecodeResult LlmDecode(LlmScorer& scorer, const Lexicon& lexicon,
                       const std::vector<std::wstring>& tokenTexts,
                       const std::unordered_set<wchar_t>& singleTokenChars,
                       const std::string& context, const std::vector<Syllable>& reading,
                       size_t beam, double variantPenalty, double lexiconWeight) {
  const size_t n = reading.size();
  // Characters each syllable can be written with, and their lexicon log-probability (natural).
  std::vector<std::unordered_map<wchar_t, double>> allowed(n);
  std::vector<std::vector<std::pair<wchar_t, double>>> lexiconChars(n);  // best first
  for (size_t i = 0; i < n; ++i) {
    std::vector<LexEntry> entries;
    lexicon.Lookup(std::span<const Syllable>(&reading[i], 1), entries);
    for (const LexEntry& e : entries) {
      if (e.text.size() != 1) continue;
      if (allowed[i].emplace(e.text[0], e.score * std::log(10.0)).second) {
        lexiconChars[i].emplace_back(e.text[0], e.score * std::log(10.0));
      }
    }
  }
  // Tokens that fit at each position: (token, characters).
  std::vector<std::vector<std::pair<int, size_t>>> fits(n);
  for (int t = 0; t < static_cast<int>(tokenTexts.size()); ++t) {
    const std::wstring& s = tokenTexts[t];
    if (s.empty() || s.size() > n) continue;
    for (size_t i = 0; i + s.size() <= n; ++i) {
      bool ok = true;
      for (size_t j = 0; j < s.size() && ok; ++j) ok = allowed[i + j].count(s[j]) > 0;
      if (ok) fits[i].emplace_back(t, s.size());
    }
  }

  struct Hyp {
    double score;  // lm + lexiconWeight * lexicon (natural log)
    std::vector<int> tokens;
    std::wstring text;
    size_t fills;
    double lm;
  };
  auto withLexicon = [&](Hyp& h) {
    if (lexiconWeight == 0) {
      h.score = h.lm;
      return;
    }
    const double lex = LexiconScore(lexicon, reading, h.text);
    // Text the lexicon cannot segment (a character only the model offered with another
    // reading split) keeps a low but finite score.
    h.score = h.lm + lexiconWeight * std::log(10.0) * (std::isinf(lex) ? -20.0 * h.text.size() : lex);
  };
  auto prune = [beam](std::vector<Hyp>& v) {
    std::sort(v.begin(), v.end(), [](const Hyp& a, const Hyp& b) { return a.score > b.score; });
    std::vector<Hyp> kept;
    for (Hyp& h : v) {
      if (kept.size() == beam) break;
      if (std::none_of(kept.begin(), kept.end(), [&](const Hyp& k) { return k.text == h.text; })) {
        kept.push_back(std::move(h));
      }
    }
    v = std::move(kept);
  };
  std::vector<std::vector<Hyp>> beams(n + 1);
  beams[0].push_back({0, {}, L"", 0, 0});
  for (size_t i = 0; i < n; ++i) {
    auto& current = beams[i];
    if (current.empty()) continue;
    prune(current);
    std::vector<std::vector<int>> prefixes;
    for (const Hyp& h : current) prefixes.push_back(h.tokens);
    std::vector<std::vector<float>> logProbs;
    if (!scorer.NextLogProbs(context, prefixes, &logProbs)) return {};
    for (size_t h = 0; h < current.size(); ++h) {
      for (const auto& [token, length] : fits[i]) {
        Hyp next{0, current[h].tokens, current[h].text + tokenTexts[token], current[h].fills,
                 current[h].lm + logProbs[h][token]};
        next.tokens.push_back(token);
        withLexicon(next);
        beams[i + length].push_back(std::move(next));
      }
      // The lexicon fills in characters the model has no single token for.
      size_t filled = 0;
      for (const auto& [ch, lexScore] : lexiconChars[i]) {
        if (filled == 3) break;
        if (singleTokenChars.count(ch)) continue;
        ++filled;
        Hyp next{0, current[h].tokens, current[h].text + ch, current[h].fills + 1,
                 current[h].lm + lexScore};
        withLexicon(next);
        const std::vector<int> pieces = scorer.Tokenize(ToUtf8(std::wstring(1, ch)), false);
        next.tokens.insert(next.tokens.end(), pieces.begin(), pieces.end());
        beams[i + 1].push_back(std::move(next));
      }
    }
    // Keep only what the next positions need (each target is pruned when it is expanded).
    for (size_t j = i + 1; j <= n; ++j) {
      if (beams[j].size() > 8 * beam) prune(beams[j]);
    }
  }
  auto& final = beams[n];
  if (final.empty()) return {};
  for (Hyp& h : final) h.score -= variantPenalty * CountNonTaiwanVariants(h.text);
  prune(final);
  return {final[0].text, final[0].fills};
}

}  // namespace

int main(int argc, char** argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::map<std::string, std::string> args = {
      {"--lexicon", "data/build/lexicons"},
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
      {"--laya-device", "CPU"},
      {"--laya-file", ""},
      {"--following-chars", "0"},
      {"--batch-layout", "auto"},
      {"--per-span", "8"},
      {"--llm-decode", "0"},
      {"--decode-lexicon-weight", "0"},  // as the IME (Ranking.cpp kOptionsPerSpan)
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
    options.device = args["--laya-device"];
    if (!args["--laya-file"].empty()) options.modelFile = FromUtf8(args["--laya-file"]);
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
  const size_t followingChars = std::stoul(args["--following-chars"]);
  const size_t decodeBeam = std::stoul(args["--llm-decode"]);
  std::vector<std::wstring> tokenTexts;
  std::unordered_set<wchar_t> singleTokenChars;
  size_t decodeCorrect = 0, decodeFills = 0, decodeFailed = 0;
  std::vector<double> decodeMs;
  size_t followingRows = 0;

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
    options.batchLayout = args["--batch-layout"];
    std::string error;
    auto loadStart = std::chrono::steady_clock::now();
    if (!scorer.Load(options, &error)) {
      fprintf(stderr, "model load failed: %s\n", error.c_str());
      return 1;
    }
    if (decodeBeam > 0) {
      tokenTexts = scorer.TokenTexts();
      for (const auto& t : tokenTexts) {
        if (t.size() == 1) singleTokenChars.insert(t[0]);
      }
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
    for (const Sentence& s : composer.NBest(k, std::stoul(args["--per-span"]))) {
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
        std::wstring following = FromUtf8(row.following).substr(0, followingChars);
        followingRows += !following.empty();
        r.lm = scorer.Score(args["--prefix"] + (useContext ? row.context : ""), candidates,
                            ToUtf8(following));
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
    if (decodeBeam > 0 && useLlm) {
      const auto start = std::chrono::steady_clock::now();
      const DecodeResult d = LlmDecode(scorer, lexicon, tokenTexts, singleTokenChars,
                                       args["--prefix"] + (useContext ? row.context : ""),
                                       row.reading, decodeBeam, 4.0,
                                       std::stod(args["--decode-lexicon-weight"]));
      decodeMs.push_back(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
      decodeCorrect += d.text == truth;
      decodeFills += d.fills;
      decodeFailed += d.text.empty();
      if (dump) dump << "#decode\t" << row.id << '\t' << ToUtf8(d.text) << '\n';
    }
    results.push_back(std::move(r));
  }

  const double n = static_cast<double>(results.size());
  size_t oracle = 0;
  for (const auto& r : results) oracle += r.truth >= 0;
  printf("rows %zu  k %zu  with following text %zu\n", results.size(), k, followingRows);
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
    if (decodeBeam > 0) {
      printf("llm-decode beam %zu  sentence %.1f%%  (lexicon fills %zu, failed %zu)  p50 %.1f ms  p95 %.1f ms\n",
             decodeBeam, 100 * decodeCorrect / n, decodeFills, decodeFailed,
             Percentile(decodeMs, 0.5), Percentile(decodeMs, 0.95));
    }
    printf("latency ms  p50 %.1f  p95 %.1f  max %.1f\n", Percentile(latencyMs, 0.5),
           Percentile(latencyMs, 0.95), Percentile(latencyMs, 1.0));
    printf("avg ms: context %.1f  candidates %.1f  softmax %.1f  (batch %.0f tokens, %.0f rows)\n",
           timingSum.context / n, timingSum.candidates / n, timingSum.softmax / n,
           timingSum.batchTokens / n, timingSum.softmaxRows / n);
  }
  return 0;
}
