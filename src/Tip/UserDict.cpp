// The user dictionary and learning, on the TIP side. LLaIME.Engine owns user.db; every TIP
// keeps a snapshot of it (shared by the text services of one process) and:
//  - fetches it again whenever the engine reports a newer version (it comes back with every
//    rank response and acknowledgement);
//  - sends what each commit taught (CollectLearning), except in private fields;
//  - adds a phrase on Ctrl+=: the range marked with Shift+Left/Right, or the whole
//    composition, and confirms it in a small notice under the text.
#include <memory>
#include <mutex>

#include "EngineClient.h"
#include "TextService.h"
#include "llaime/Learning.h"
#include "ui/NoticeWindow.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

struct SharedUserDict {
  std::mutex mutex;
  uint32_t version = 0;
  bool learningEnabled = true;
  std::shared_ptr<const UserDictionary> dict = std::make_shared<UserDictionary>();
};

SharedUserDict& Shared() {
  static SharedUserDict shared;
  return shared;
}

std::vector<Syllable> ToSyllables(const protocol::Reading& reading) {
  std::vector<Syllable> out;
  for (uint16_t bits : reading) out.push_back(Syllable(bits));
  return out;
}

protocol::Reading ToWire(const std::vector<Syllable>& reading) {
  protocol::Reading out;
  for (Syllable s : reading) out.push_back(s.bits());
  return out;
}

}  // namespace

void TextService::RequestUserDictionary() {
  // One request at a time; the client gives up on an absent engine after ~30 s.
  constexpr ULONGLONG kRequestTimeoutMs = 40'000;
  const ULONGLONG now = GetTickCount64();
  if (!engine_ || (userDictRequestTick_ && now - userDictRequestTick_ < kRequestTimeoutMs)) return;
  userDictRequestTick_ = now;
  protocol::UserDictRequest request;
  {
    std::lock_guard<std::mutex> lock(Shared().mutex);
    request.knownVersion = Shared().version;
  }
  engine_->Send(protocol::Encode(request));
}

void TextService::CheckUserVersion(uint32_t version) {
  if (version == 0) return;
  {
    std::lock_guard<std::mutex> lock(Shared().mutex);
    if (Shared().version == version) {
      ApplyUserDictionary();  // another text service in this process may have fetched it
      return;
    }
  }
  RequestUserDictionary();
}

void TextService::OnUserDictResponse(const protocol::UserDictResponse& response) {
  userDictRequestTick_ = 0;
  if (response.changed) {
    auto dict = std::make_shared<UserDictionary>();
    for (const auto& p : response.phrases) {
      dict->AddPhrase(ToSyllables(p.reading), p.text,
                      p.kind == 1 ? UserDictionary::PhraseKind::Learned
                                  : UserDictionary::PhraseKind::Manual);
    }
    for (const auto& s : response.selections) {
      dict->AddSelection(ToSyllables(s.reading), s.text, s.prev, s.weight);
    }
    std::lock_guard<std::mutex> lock(Shared().mutex);
    Shared().version = response.version;
    Shared().learningEnabled = response.learningEnabled;
    Shared().dict = std::move(dict);
  }
  ApplyUserDictionary();
}

void TextService::ApplyUserDictionary() {
  // Swapping it mid-composition would re-convert what is on screen; wait for the next one.
  if (IsComposing()) return;
  std::lock_guard<std::mutex> lock(Shared().mutex);
  learningEnabled_ = Shared().learningEnabled;
  if (userDict_ == Shared().dict) return;
  userDict_ = Shared().dict;
  composer_.SetUserDictionary(userDict_.get());
}

void TextService::OnAck(const protocol::Ack& ack) {
  if (ack.request == protocol::MessageType::PhraseRequest && notice_) {
    notice_->Show(ack.ok ? L"已加入「" + ack.text + L"」" : L"無法加入「" + ack.text + L"」",
                  noticeAnchor_);
  }
  CheckUserVersion(ack.userVersion);
}

void TextService::LearnFromCommit(size_t upTo) {
  if (!engine_ || privateMode_ || !learningEnabled_) return;
  LearningEvents events = CollectLearning(composer_, upTo);
  if (events.empty()) return;
  protocol::LearnRequest request;
  for (auto& s : events.selections) {
    request.selections.push_back({ToWire(s.reading), std::move(s.text), std::move(s.prev)});
  }
  for (auto& p : events.phrases) request.phrases.push_back({ToWire(p.reading), std::move(p.text)});
  engine_->Send(protocol::Encode(request));
}

void TextService::SetLearningEnabled(bool enabled) {
  if (!engine_) return;
  learningEnabled_ = enabled;
  {
    std::lock_guard<std::mutex> lock(Shared().mutex);
    Shared().learningEnabled = enabled;
  }
  protocol::SetLearningRequest request;
  request.enabled = enabled;
  engine_->Send(protocol::Encode(request));
}

std::pair<size_t, size_t> TextService::MarkedRange() const {
  if (!markAnchor_) return {0, 0};
  const size_t a = std::min(*markAnchor_, composer_.size()), b = composer_.cursor();
  return {std::min(a, b), std::max(a, b)};
}

void TextService::MoveMark(int delta) {
  if (!reading_.Empty()) return;
  if (!markAnchor_) markAnchor_ = composer_.cursor();
  const size_t cursor = composer_.cursor();
  if (delta < 0 && cursor > 0) composer_.SetCursor(cursor - 1);
  if (delta > 0) composer_.SetCursor(cursor + 1);
}

void TextService::AddPhraseFromComposition(ITfContext* context, TfEditCookie ec) {
  if (!notice_) return;
  auto [start, end] = MarkedRange();
  if (start == end) {
    start = 0;
    end = composer_.size();
  }
  const size_t length = end - start;
  // The text over [start, end): whole nodes, or characters of nodes with one per syllable.
  std::wstring text;
  bool ok = reading_.Empty() && length >= 1 && length <= Lexicon::kMaxPhraseLength &&
            composer_.IsReadingSpan(start, length);
  for (const Node& node : composer_.Walk()) {
    if (!ok) break;
    const size_t nodeEnd = node.start + node.length;
    if (nodeEnd <= start || node.start >= end) continue;
    if (node.start >= start && nodeEnd <= end) {
      text += node.text;
    } else if (node.text.size() == node.length) {
      const size_t from = std::max(start, node.start), to = std::min(end, nodeEnd);
      text += node.text.substr(from - node.start, to - from);
    } else {
      ok = false;
    }
  }
  const size_t begin = composer_.TextOffsetOf(start);
  if (!GetTextRect(context, ec, begin, begin + 1, &noticeAnchor_)) {
    GetTextRect(context, ec, 0, 1, &noticeAnchor_);
  }
  if (!ok || text.empty()) {
    notice_->Show(L"用 Shift+←→ 選取 1 到 9 個字，再按 Ctrl+= 加入詞庫", noticeAnchor_);
    return;
  }
  protocol::PhraseRequest request;
  request.op = protocol::PhraseRequest::Add;
  request.reading = ToWire(composer_.Syllables(start, length));
  request.text = text;
  engine_->Send(protocol::Encode(request));
  // Keep showing the phrase as one word.
  composer_.Select({text, start, length, UserDictionary::kPhraseScore});
  markAnchor_.reset();
}

}  // namespace llaime
