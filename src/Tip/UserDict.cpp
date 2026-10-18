// The user dictionary and learning, on the TIP side. LLaIME.Engine owns user.db; every TIP
// keeps a snapshot of it (shared by the text services of one process) and:
//  - fetches it again whenever the engine reports a newer version (it comes back with every
//    rank response and acknowledgement);
//  - sends what each commit taught (CollectLearning), except in private fields.
// Phrases are added and removed in the settings app (stage 7), which edits user.db directly.
#include <memory>
#include <mutex>

#include "EngineClient.h"
#include "TextService.h"
#include "llaime/Learning.h"

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
    for (const auto& s : response.shortcuts) dict->AddShortcut(s.keys, s.text);
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
  // Lexicon layers turned on or off in the settings are picked up at the same points.
  ApplyLexicon();
  std::lock_guard<std::mutex> lock(Shared().mutex);
  learningEnabled_ = Shared().learningEnabled;
  if (userDict_ == Shared().dict) return;
  userDict_ = Shared().dict;
  composer_.SetUserDictionary(userDict_.get());
}

void TextService::OnAck(const protocol::Ack& ack) { CheckUserVersion(ack.userVersion); }

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

}  // namespace llaime
