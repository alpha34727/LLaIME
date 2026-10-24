#include "LlmDecoder.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace llaime {
namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();
const double kLn10 = std::log(10.0);
// Lexicon characters per syllable filled in when the model has no token of their own.
constexpr size_t kFillChars = 3;
// A target position holds at most this many hypotheses (times the beam) before it is pruned.
constexpr size_t kPendingFactor = 8;

std::string Utf8(const std::wstring& s) {
  if (s.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                    nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

}  // namespace

LlmDecoder::LlmDecoder(LlmScorer* scorer) : scorer_(scorer) {
  tokenTexts_ = scorer_->TokenTexts();
  for (int t = 0; t < static_cast<int>(tokenTexts_.size()); ++t) {
    const std::wstring& s = tokenTexts_[t];
    if (s.empty()) continue;
    tokensByFirst_[s[0]].push_back(t);
    if (s.size() == 1) singleTokenChars_.insert(s[0]);
  }
}

void LlmDecoder::Prune(std::vector<Hyp>& hyps, size_t beam) const {
  std::sort(hyps.begin(), hyps.end(), [](const Hyp& a, const Hyp& b) { return a.score > b.score; });
  std::vector<Hyp> kept;
  for (Hyp& h : hyps) {
    if (kept.size() == beam) break;
    if (std::none_of(kept.begin(), kept.end(), [&](const Hyp& k) { return k.text == h.text; })) {
      kept.push_back(std::move(h));
    }
  }
  hyps = std::move(kept);
}

void LlmDecoder::Extend(Hyp& h, size_t from, const std::wstring& text,
                        const std::vector<Arc>& lattice, bool oneElement) const {
  h.text += text;
  const size_t elements = oneElement ? 1 : text.size();
  for (size_t k = 1; k <= elements; ++k) {
    const size_t e = from + k;
    h.offset.push_back(h.offset[from] + static_cast<int>(oneElement ? text.size() : k));
    double best = kNegInf;
    int back = -1;
    for (uint32_t a : endingAt_[e]) {
      const Arc& arc = lattice[a];
      if (arc.forced) continue;  // taken only as a whole
      const int start = h.offset[arc.start];
      if (start < 0 || h.best[arc.start] == kNegInf) continue;
      if (h.offset[e] - start != static_cast<int>(arc.text.size())) continue;
      if (h.text.compare(start, arc.text.size(), arc.text) != 0) continue;
      if (h.best[arc.start] + arc.score > best) {
        best = h.best[arc.start] + arc.score;
        back = static_cast<int>(a);
      }
    }
    h.best.push_back(best);
    h.back.push_back(back);
  }
}

void LlmDecoder::Rescore(Hyp& h, double lexiconWeight) const {
  const double lexicon = h.best.back();
  // Text no lexicon segmentation covers keeps a low but finite score.
  h.score = h.lm + lexiconWeight * kLn10 *
                       (lexicon == kNegInf ? -20.0 * static_cast<double>(h.text.size()) : lexicon);
}

std::vector<LlmDecoder::Path> LlmDecoder::Decode(const std::string& context,
                                                 const std::string& following, size_t elements,
                                                 const std::vector<Arc>& lattice, size_t beam,
                                                 double lexiconWeight) {
  const size_t n = elements;
  beam = std::clamp<size_t>(beam, 1, 16);
  if (n == 0) return {};

  // Single characters the model may write at each element (best first); the lexicon's choices
  // for one element the model has no single token for (rare characters outside the BMP, emoji,
  // characters it splits into bytes), best first; and arcs taken whole (pins, punctuation).
  std::vector<std::vector<std::pair<wchar_t, uint32_t>>> freeOrder(n);
  std::vector<std::unordered_set<wchar_t>> allowed(n);
  std::vector<std::vector<uint32_t>> fillOrder(n);
  std::vector<std::vector<uint32_t>> forcedAt(n);
  endingAt_.assign(n + 1, {});
  std::vector<uint64_t> signature(n, 0xcbf29ce484222325ull);
  for (uint32_t a = 0; a < lattice.size(); ++a) {
    const Arc& arc = lattice[a];
    if (arc.length == 0 || arc.start + arc.length > n) continue;
    endingAt_[arc.start + arc.length].push_back(a);
    uint64_t& sig = signature[arc.start + arc.length - 1];
    sig = Mix(Mix(Mix(sig, arc.start), arc.length * 2 + (arc.forced ? 1 : 0)),
              static_cast<uint64_t>(std::llround(arc.score * 1e4)));
    for (wchar_t c : arc.text) sig = Mix(sig, c);
    if (arc.forced) {
      forcedAt[arc.start].push_back(a);
    } else if (arc.length == 1 && arc.text.size() == 1) {
      if (allowed[arc.start].insert(arc.text[0]).second) {
        freeOrder[arc.start].emplace_back(arc.text[0], a);
        if (!singleTokenChars_.count(arc.text[0])) fillOrder[arc.start].push_back(a);
      }
    } else if (arc.length == 1 && !arc.text.empty()) {
      fillOrder[arc.start].push_back(a);
    }
    // Longer arcs whose text is not one character per element (emoji for a phrase) take part
    // in no conversion.
  }
  // An element nothing above covers (a syllable the lexicon does not know) is taken as is.
  for (uint32_t a = 0; a < lattice.size(); ++a) {
    const Arc& arc = lattice[a];
    if (arc.length == 1 && arc.start < n && !arc.forced && freeOrder[arc.start].empty() &&
        fillOrder[arc.start].empty()) {
      forcedAt[arc.start].push_back(a);
    }
  }
  auto fits = [&](size_t i, const std::wstring& s) {
    if (i + s.size() > n) return false;
    for (size_t k = 0; k < s.size(); ++k) {
      if (!allowed[i + k].count(s[k])) return false;
    }
    return true;
  };

  // Keep what the previous call decoded for the positions whose lattice is unchanged.
  size_t reuse = 0;
  if (context == cachedContext_ && lexiconWeight == cachedWeight_ && beam == cachedBeam_) {
    while (reuse < n && reuse < cachedSignature_.size() && signature[reuse] == cachedSignature_[reuse]) {
      ++reuse;
    }
  }
  std::vector<std::vector<Hyp>> beams(n + 1);
  std::vector<Position> positions(n);
  if (reuse == 0) {
    Hyp start;
    start.offset = {0};
    start.best = {0};
    start.back = {-1};
    beams[0].push_back(std::move(start));
  } else {
    // Positions before `reuse` were expanded with the same lattice; the hypotheses reaching
    // `reuse` are complete too (they only depend on earlier elements).
    for (size_t i = 0; i < reuse; ++i) positions[i] = std::move(cachedPositions_[i]);
    beams[reuse] = reuse < cachedPositions_.size() ? std::move(cachedPositions_[reuse].hyps)
                                                   : std::move(cachedFinal_);
    // Transitions from those positions that now reach past `reuse`.
    for (size_t i = 0; i < reuse; ++i) {
      const Position& p = positions[i];
      for (size_t h = 0; h < p.hyps.size(); ++h) {
        for (const auto& [token, logProb] : p.next[h]) {
          const std::wstring& s = tokenTexts_[token];
          if (i + s.size() <= reuse || !fits(i, s)) continue;
          Hyp x = p.hyps[h];
          x.tokens.push_back(token);
          x.lm += logProb;
          Extend(x, i, s, lattice);
          Rescore(x, lexiconWeight);
          beams[i + s.size()].push_back(std::move(x));
        }
      }
    }
  }
  cachedPositions_.clear();
  cachedFinal_.clear();
  cachedSignature_.clear();

  auto forced = [&](const Hyp& from, size_t i, uint32_t a) {
    const Arc& arc = lattice[a];
    Hyp x = from;
    const std::vector<int> pieces = scorer_->Tokenize(Utf8(arc.text), false);
    x.tokens.insert(x.tokens.end(), pieces.begin(), pieces.end());
    x.text += arc.text;
    for (size_t k = 1; k < arc.length; ++k) {
      x.offset.push_back(-1);
      x.best.push_back(kNegInf);
      x.back.push_back(-1);
    }
    x.offset.push_back(from.offset[i] + static_cast<int>(arc.text.size()));
    x.best.push_back(from.best[i] == kNegInf ? kNegInf : from.best[i] + arc.score);
    x.back.push_back(static_cast<int>(a));
    Rescore(x, lexiconWeight);
    return x;
  };
  if (reuse > 0) {
    for (size_t i = 0; i < reuse; ++i) {
      for (uint32_t a : forcedAt[i]) {
        if (i + lattice[a].length <= reuse) continue;
        for (const Hyp& h : positions[i].hyps) beams[i + lattice[a].length].push_back(forced(h, i, a));
      }
    }
  }

  for (size_t i = reuse; i < n; ++i) {
    Prune(beams[i], beam);
    Position& p = positions[i];
    p.hyps = std::move(beams[i]);
    p.next.assign(p.hyps.size(), {});
    if (p.hyps.empty()) continue;
    std::vector<std::vector<int>> prefixes;
    for (const Hyp& h : p.hyps) prefixes.push_back(h.tokens);
    std::vector<std::vector<float>> logProbs;
    if (!scorer_->NextLogProbs(context, prefixes, &logProbs)) return {};
    for (size_t h = 0; h < p.hyps.size(); ++h) {
      for (const auto& [ch, arc] : freeOrder[i]) {
        auto tokens = tokensByFirst_.find(ch);
        if (tokens == tokensByFirst_.end()) continue;
        for (int t : tokens->second) p.next[h].emplace_back(t, logProbs[h][t]);
      }
      for (const auto& [token, logProb] : p.next[h]) {
        const std::wstring& s = tokenTexts_[token];
        if (!fits(i, s)) continue;
        Hyp x = p.hyps[h];
        x.tokens.push_back(token);
        x.lm += logProb;
        Extend(x, i, s, lattice);
        Rescore(x, lexiconWeight);
        beams[i + s.size()].push_back(std::move(x));
      }
      for (size_t f = 0; f < fillOrder[i].size() && f < kFillChars; ++f) {
        const Arc& arc = lattice[fillOrder[i][f]];
        Hyp x = p.hyps[h];
        const std::vector<int> pieces = scorer_->Tokenize(Utf8(arc.text), false);
        x.tokens.insert(x.tokens.end(), pieces.begin(), pieces.end());
        x.lm += arc.score * kLn10;
        Extend(x, i, arc.text, lattice, true);
        Rescore(x, lexiconWeight);
        beams[i + 1].push_back(std::move(x));
      }
      for (uint32_t a : forcedAt[i]) beams[i + lattice[a].length].push_back(forced(p.hyps[h], i, a));
    }
    for (size_t j = i + 1; j <= n; ++j) {
      if (beams[j].size() > kPendingFactor * beam) Prune(beams[j], beam);
    }
  }

  cachedContext_ = context;
  cachedWeight_ = lexiconWeight;
  cachedBeam_ = beam;
  cachedSignature_ = signature;
  cachedFinal_ = beams[n];
  std::vector<Hyp> finals = std::move(beams[n]);
  cachedPositions_ = std::move(positions);

  Prune(finals, beam);
  if (!following.empty() && !finals.empty()) {
    // The text after the composition: log P(text + following | context) for the finalists.
    std::vector<std::string> texts;
    for (const Hyp& h : finals) texts.push_back(Utf8(h.text));
    const std::vector<double> scores = scorer_->Score(context, texts, following);
    if (scores.size() == finals.size()) {
      for (size_t k = 0; k < finals.size(); ++k) {
        finals[k].lm = scores[k];
        Rescore(finals[k], lexiconWeight);
      }
      std::sort(finals.begin(), finals.end(),
                [](const Hyp& a, const Hyp& b) { return a.score > b.score; });
    }
  }

  std::vector<Path> paths;
  for (const Hyp& h : finals) {
    Path path;
    path.lm = h.lm;
    path.lexicon = h.best[n];
    bool ok = h.best[n] != kNegInf;
    for (size_t e = n; ok && e > 0;) {
      const int a = h.back[e];
      if (a < 0) {
        ok = false;
        break;
      }
      path.arcs.push_back(static_cast<uint32_t>(a));
      e = lattice[a].start;
    }
    if (!ok) continue;
    std::reverse(path.arcs.begin(), path.arcs.end());
    paths.push_back(std::move(path));
  }
  return paths;
}

}  // namespace llaime
