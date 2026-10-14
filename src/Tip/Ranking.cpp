// Language-model reranking: after each edit the lexicon's best conversion is shown at once,
// and the N best conversions are sent to the engine; when its scores come back for the same
// buffer state, the best combined conversion replaces the displayed one.
#include <cmath>

#include "EditSession.h"
#include "llaime/TaiwanUsage.h"
#include "EngineClient.h"
#include "TextService.h"
#include "ui/CandidateWindow.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

constexpr size_t kCandidates = 16;
// Combined score = LM log-prob + kLexiconWeight * lexicon log-prob (natural log)
// - kVariantPenalty per non-Taiwan-standard spelling (身份, 計劃, ...), which general models
// prefer. Tuned with LLaIME.Eval on tests/eval (Common Voice dev and taiwan_usage).
constexpr double kLexiconWeight = 0.75;
constexpr double kVariantPenalty = 4.0;
// Characters before the composition given to the model as context.
constexpr LONG kContextChars = 32;

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

void TextService::OnRankResponse(const protocol::RankResponse& response) {
  if (response.requestId != lastRequestId_ || composer_.generation() != rankedGeneration_ ||
      response.scores.size() != rankedSentences_.size()) {
    return;  // the buffer changed since the request
  }
  size_t best = 0;
  double bestScore = -1e300;
  for (size_t i = 0; i < rankedSentences_.size(); ++i) {
    const double score = response.scores[i] +
                         kLexiconWeight * rankedSentences_[i].score * std::log(10.0) -
                         kVariantPenalty * CountNonTaiwanVariants(rankedSentences_[i].Text());
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
