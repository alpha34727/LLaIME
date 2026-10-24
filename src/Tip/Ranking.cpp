// Language-model reranking: after each edit the lexicon's best conversion is shown at once,
// and the N best conversions are sent to the engine; when its scores come back for the same
// buffer state, the best combined conversion replaces the displayed one.
//
// While the candidate list is open, each candidate is scored the same way through the sentence
// it would produce, and the softmax of those scores is shown as its probability; candidates of
// 1% or more are then listed by probability.
#include <algorithm>
#include <cmath>

#include "EditSession.h"
#include "llaime/ImeSettings.h"
#include "llaime/Learning.h"
#include "llaime/TaiwanUsage.h"
#include "EngineClient.h"
#include "TextService.h"
#include "ui/CandidateWindow.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

constexpr size_t kCandidates = 16;  // the engine scores at most this many per request
// Lexicon entries per span the N best conversions are built from: with 4, a homophone ranked
// 5th by the lexicon could never be chosen automatically.
constexpr size_t kOptionsPerSpan = 8;
// Combined score = LM log-prob + kLexiconWeight * lexicon log-prob (natural log)
// - kVariantPenalty per non-Taiwan-standard spelling (身份, 計劃, ...), which general models
// prefer, + the user's picks among its words (LearnedBonus). Tuned with LLaIME.Eval on
// tests/eval (Common Voice dev and taiwan_usage).
constexpr double kLexiconWeight = 0.75;
constexpr double kVariantPenalty = 4.0;
// Open candidates at or above this probability are listed by probability.
constexpr double kRankedProbability = 0.01;
// With "LLM 寫字比重" above 0 the model writes the conversion over the composer's lattice
// (LlmDecoder in the engine): this many hypotheses per position, every character of a syllable
// up to kDecodeChars, kDecodePhrases entries per longer span. The lexicon's weight goes from
// kMaxDecodeLexiconWeight (1%) to 0 (100%, the model alone); 50% gives the 0.75 that tested
// best (tests/eval/RESULTS.md).
constexpr uint32_t kDecodeBeam = 8;
constexpr size_t kDecodeChars = 200;
constexpr size_t kDecodePhrases = 16;
constexpr double kMaxDecodeLexiconWeight = 1.5;

double CombinedScore(double lm, const Sentence& sentence, const Composer& composer,
                     const UserDictionary* user) {
  return lm + kLexiconWeight * sentence.score * std::log(10.0) -
         kVariantPenalty * CountNonTaiwanVariants(sentence.Text()) +
         (user ? LearnedBonus(*user, composer, sentence.nodes) : 0.0);
}

}  // namespace

void TextService::ReadContext(ITfContext*, TfEditCookie ec, ITfRange* compositionRange) {
  contextText_.clear();
  followingText_.clear();
  if (privateMode_) return;  // never send what was typed into a private field
  // Up to the set number of characters on each side (0: that side is not read); an app that
  // does not expose the text after the insertion point gives none.
  const ContextChars chars = CurrentContextChars();
  auto read = [&](TfAnchor anchor, LONG contextChars, std::wstring* out) {
    if (contextChars == 0) return;
    ComPtr<ITfRange> range;
    if (FAILED(compositionRange->Clone(&range))) return;
    range->Collapse(ec, anchor);
    LONG shifted = 0;
    if (anchor == TF_ANCHOR_START) {
      range->ShiftStart(ec, -contextChars, &shifted, nullptr);
    } else {
      range->ShiftEnd(ec, contextChars, &shifted, nullptr);
    }
    std::wstring buf(contextChars, L'\0');
    ULONG got = 0;
    if (SUCCEEDED(range->GetText(ec, 0, buf.data(), contextChars, &got))) out->assign(buf, 0, got);
  };
  read(TF_ANCHOR_START, static_cast<LONG>(chars.before), &contextText_);
  read(TF_ANCHOR_END, static_cast<LONG>(chars.after), &followingText_);
}

void TextService::RequestRanking() {
  // A single syllable is ranked too: with the text around it, the model picks the right
  // homophone far more often than the lexicon's order (tests/eval/*_short.tsv).
  if (!engine_ || composer_.size() == 0) return;
  rankedSentences_ = composer_.NBest(kCandidates, kOptionsPerSpan);
  if (rankedSentences_.size() < 2) return;
  rankedGeneration_ = composer_.generation();

  protocol::RankRequest request;
  request.requestId = ++lastRequestId_;
  request.privateMode = privateMode_;
  request.context = privateMode_ ? std::wstring() : contextText_;
  request.following = privateMode_ ? std::wstring() : followingText_;
  for (const Sentence& s : rankedSentences_) request.candidates.push_back(s.Text());
  // The candidates stay in the request: without a language model (Laya, or still loading) the
  // engine ranks them instead.
  decodeLattice_.clear();
  if (const uint32_t writing = CurrentLlmWriting(); writing > 0) {
    decodeLattice_ = composer_.Lattice(kDecodeChars, kDecodePhrases);
    decodeLexiconWeight_ = kMaxDecodeLexiconWeight * (kMaxLlmWriting - writing) / kMaxLlmWriting;
    request.decodeBeam = kDecodeBeam;
    request.decodeLexiconWeight = decodeLexiconWeight_;
    request.elements = static_cast<uint32_t>(composer_.size());
    request.arcs.reserve(decodeLattice_.size());
    for (const Node& node : decodeLattice_) {
      // Pins and punctuation are taken as they are.
      const bool forced = node.pinned || !composer_.IsReadingSpan(node.start, node.length);
      request.arcs.push_back({static_cast<uint32_t>(node.start), static_cast<uint32_t>(node.length),
                              forced, node.score, node.text});
    }
  }
  engine_->Submit(std::move(request));
}

// The conversion the model wrote (RankResponse::paths): the best path after the user's picks
// and the non-Taiwan-spelling penalty, as a sentence over the lattice sent.
bool TextService::DecodedSentence(const protocol::RankResponse& response, Sentence* out) const {
  double best = -1e300;
  bool found = false;
  for (const auto& path : response.paths) {
    Sentence sentence;
    size_t next = 0;
    bool ok = true;
    for (uint32_t a : path.arcs) {
      if (a >= decodeLattice_.size() || decodeLattice_[a].start != next) {
        ok = false;
        break;
      }
      sentence.nodes.push_back(decodeLattice_[a]);
      sentence.score += decodeLattice_[a].score;
      next += decodeLattice_[a].length;
    }
    if (!ok || next != composer_.size()) continue;
    const double score = path.lm + decodeLexiconWeight_ * std::log(10.0) * path.lexicon -
                         kVariantPenalty * CountNonTaiwanVariants(sentence.Text()) +
                         (userDict_ ? LearnedBonus(*userDict_, composer_, sentence.nodes) : 0.0);
    if (score > best) {
      best = score;
      *out = std::move(sentence);
      found = true;
    }
  }
  return found;
}

bool TextService::RequestCandidateProbabilities() {
  candidateRequestId_ = 0;
  candidateSentences_.clear();
  candidateSentenceIndex_.assign(candidates_.size(), -1);
  if (!engine_) return false;
  for (size_t i = 0; i < candidates_.size(); ++i) {
    const Candidate& c = candidates_[i];
    // The best conversion with this candidate chosen. Its span is pinned in the copy, and a
    // pin scores 0, so the candidate's own lexicon score is added back.
    Composer chosen = composer_;
    chosen.Select(c);
    Sentence sentence;
    sentence.nodes = chosen.Walk();
    for (const Node& node : sentence.nodes) {
      sentence.score += node.start == c.start && node.length == c.length ? c.score : node.score;
    }
    const std::wstring text = sentence.Text();
    auto same = std::find_if(candidateSentences_.begin(), candidateSentences_.end(),
                             [&](const Sentence& s) { return s.Text() == text; });
    if (same != candidateSentences_.end()) {
      // Choosing this candidate gives the same sentence as an earlier (longer) one, e.g. 服
      // then 飾 for 服飾: it shares that sentence's probability.
      candidateSentenceIndex_[i] = static_cast<int>(same - candidateSentences_.begin());
      continue;
    }
    if (candidateSentences_.size() == kCandidates) continue;  // past what one request scores
    candidateSentenceIndex_[i] = static_cast<int>(candidateSentences_.size());
    candidateSentences_.push_back(std::move(sentence));
  }
  if (candidateSentences_.size() < 2) return false;

  protocol::RankRequest request;
  request.requestId = ++lastRequestId_;
  request.privateMode = privateMode_;
  request.context = privateMode_ ? std::wstring() : contextText_;
  request.following = privateMode_ ? std::wstring() : followingText_;
  for (const Sentence& s : candidateSentences_) request.candidates.push_back(s.Text());
  candidateRequestId_ = request.requestId;
  rankedGeneration_ = composer_.generation();
  engine_->Submit(std::move(request));
  return true;
}

void TextService::OnCandidateProbabilities(const protocol::RankResponse& response) {
  if (!CandidatesOpen() || composer_.generation() != rankedGeneration_ ||
      response.scores.size() != candidateSentences_.size()) {
    return;
  }
  std::vector<double> scores(candidateSentences_.size());
  double best = -1e300;
  for (size_t i = 0; i < scores.size(); ++i) {
    scores[i] =
        CombinedScore(response.scores[i], candidateSentences_[i], composer_, userDict_.get());
    best = std::max(best, scores[i]);
  }
  double sum = 0;
  for (double& s : scores) sum += (s = std::exp(s - best));
  std::vector<double> probabilities(candidates_.size(), -1.0);
  for (size_t i = 0; i < candidates_.size(); ++i) {
    const int index = candidateSentenceIndex_[i];
    // Candidates past what one request scores stay unknown (no bar).
    if (index >= 0) probabilities[i] = scores[index] / sum;
  }

  // Candidates the model gives at least kRankedProbability go first, most likely first (ties
  // keep the lexicon order, longer phrases first); the rest keep the lexicon order after them.
  std::vector<size_t> order(candidates_.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_partition(order.begin(), order.end(),
                        [&](size_t i) { return probabilities[i] >= kRankedProbability; });
  const auto ranked = std::find_if(order.begin(), order.end(), [&](size_t i) {
    return probabilities[i] < kRankedProbability;
  });
  std::stable_sort(order.begin(), ranked,
                   [&](size_t a, size_t b) { return probabilities[a] > probabilities[b]; });

  std::vector<Candidate> candidates(candidates_.size());
  std::vector<int> sentenceIndex(candidates_.size());
  std::vector<double> reordered(candidates_.size());
  for (size_t i = 0; i < order.size(); ++i) {
    candidates[i] = std::move(candidates_[order[i]]);
    sentenceIndex[i] = candidateSentenceIndex_[order[i]];
    reordered[i] = probabilities[order[i]];
  }
  candidates_ = std::move(candidates);
  candidateSentenceIndex_ = std::move(sentenceIndex);
  candidateRequestId_ = 0;  // applied once
  candidateWindow_->SetProbabilities(std::move(reordered), order);
  UpdateCandidateUi();
}

void TextService::OnRankResponse(const protocol::RankResponse& response) {
  CheckUserVersion(response.userVersion);
  if (!response.ok) return;  // the engine is still loading its model
  if (response.requestId != 0 && response.requestId == candidateRequestId_) {
    if (response.ok) OnCandidateProbabilities(response);
    return;
  }
  if (response.requestId != lastRequestId_ || composer_.generation() != rankedGeneration_) {
    return;  // the buffer changed since the request
  }
  Sentence chosen;
  if (!response.paths.empty()) {
    if (!DecodedSentence(response, &chosen)) return;
  } else {
    if (response.scores.size() != rankedSentences_.size()) return;
    size_t best = 0;
    double bestScore = -1e300;
    for (size_t i = 0; i < rankedSentences_.size(); ++i) {
      const double score =
          CombinedScore(response.scores[i], rankedSentences_[i], composer_, userDict_.get());
      if (score > bestScore) {
        bestScore = score;
        best = i;
      }
    }
    chosen = rankedSentences_[best];
  }
  if (chosen.Text() == composer_.Text()) return;
  composer_.SetPreferred(chosen);
  // Leave an open candidate list alone; the new conversion shows once it closes.
  if (CandidatesOpen() || !compositionContext_) return;
  ComPtr<ITfContext> context = compositionContext_;
  ComPtr<ITfComposition> composition = composition_;
  ComPtr<TextService> self(this);
  EditSession::Request(context.Get(), clientId_, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                       [self, context, composition](TfEditCookie ec) {
                         // Granted later: only if that composition is still ours, and
                         // never to start a new one.
                         if (self->composition_ == composition && composition &&
                             self->compositionContext_ == context) {
                           self->UpdateComposition(context.Get(), ec);
                         }
                         return S_OK;
                       });
}

}  // namespace llaime
