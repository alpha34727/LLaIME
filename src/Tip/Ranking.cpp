// Language-model reranking: after each edit the lexicon's best conversion is shown at once,
// and the N best conversions are sent to the engine; when its scores come back for the same
// buffer state, the best combined conversion replaces the displayed one.
//
// While the candidate list is open, each candidate is scored the same way through the sentence
// it would produce, and the softmax of those scores is shown as its probability.
#include <algorithm>
#include <cmath>

#include "EditSession.h"
#include "llaime/TaiwanUsage.h"
#include "EngineClient.h"
#include "TextService.h"
#include "ui/CandidateWindow.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

constexpr size_t kCandidates = 16;  // the engine scores at most this many per request
// Combined score = LM log-prob + kLexiconWeight * lexicon log-prob (natural log)
// - kVariantPenalty per non-Taiwan-standard spelling (身份, 計劃, ...), which general models
// prefer. Tuned with LLaIME.Eval on tests/eval (Common Voice dev and taiwan_usage).
constexpr double kLexiconWeight = 0.75;
constexpr double kVariantPenalty = 4.0;
// Characters before the composition given to the model as context.
constexpr LONG kContextChars = 32;

double CombinedScore(double lm, const Sentence& sentence) {
  return lm + kLexiconWeight * sentence.score * std::log(10.0) -
         kVariantPenalty * CountNonTaiwanVariants(sentence.Text());
}

}  // namespace

void TextService::ReadContextBefore(ITfContext*, TfEditCookie ec, ITfRange* compositionRange) {
  contextText_.clear();
  if (privateMode_) return;  // never send what was typed into a private field
  ComPtr<ITfRange> before;
  if (FAILED(compositionRange->Clone(&before))) return;
  before->Collapse(ec, TF_ANCHOR_START);
  LONG shifted = 0;
  before->ShiftStart(ec, -kContextChars, &shifted, nullptr);
  wchar_t buf[kContextChars + 1] = {};
  ULONG got = 0;
  if (SUCCEEDED(before->GetText(ec, 0, buf, kContextChars, &got))) contextText_.assign(buf, got);
}

void TextService::RequestRanking() {
  if (!engine_ || composer_.size() < 2) return;
  rankedSentences_ = composer_.NBest(kCandidates);
  if (rankedSentences_.size() < 2) return;
  rankedGeneration_ = composer_.generation();

  protocol::RankRequest request;
  request.requestId = ++lastRequestId_;
  request.privateMode = privateMode_;
  request.context = privateMode_ ? std::wstring() : contextText_;
  for (const Sentence& s : rankedSentences_) request.candidates.push_back(s.Text());
  engine_->Submit(std::move(request));
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
    scores[i] = CombinedScore(response.scores[i], candidateSentences_[i]);
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
  candidateWindow_->SetProbabilities(std::move(probabilities));
}

void TextService::OnRankResponse(const protocol::RankResponse& response) {
  if (response.requestId != 0 && response.requestId == candidateRequestId_) {
    if (response.ok) OnCandidateProbabilities(response);
    return;
  }
  if (response.requestId != lastRequestId_ || composer_.generation() != rankedGeneration_ ||
      response.scores.size() != rankedSentences_.size()) {
    return;  // the buffer changed since the request
  }
  size_t best = 0;
  double bestScore = -1e300;
  for (size_t i = 0; i < rankedSentences_.size(); ++i) {
    const double score = CombinedScore(response.scores[i], rankedSentences_[i]);
    if (score > bestScore) {
      bestScore = score;
      best = i;
    }
  }
  if (rankedSentences_[best].Text() == composer_.Text()) return;
  composer_.SetPreferred(rankedSentences_[best]);
  // Leave an open candidate list alone; the new conversion shows once it closes.
  if (CandidatesOpen() || !compositionContext_) return;
  ComPtr<ITfContext> context = compositionContext_;
  EditSession::Request(context.Get(), clientId_, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                       [this, context](TfEditCookie ec) {
                         UpdateComposition(context.Get(), ec);
                         return S_OK;
                       });
}

}  // namespace llaime
