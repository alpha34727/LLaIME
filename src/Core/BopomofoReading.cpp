#include "llaime/BopomofoReading.h"

#include <array>

namespace llaime {
namespace {

using Kind = BopomofoKey::Kind;

class StandardLayout final : public BopomofoLayout {
 public:
  std::optional<BopomofoKey> Map(wchar_t c) const override {
    // 大千 layout: keys listed in ㄅㄆㄇㄈ... order.
    static constexpr std::wstring_view kConsonantKeys = L"1qaz2wsxedcrfv5tgbyhn";
    static constexpr std::wstring_view kMedialKeys = L"ujm";
    static constexpr std::wstring_view kVowelKeys = L"8ik,9ol.0p;/-";
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    if (auto i = kConsonantKeys.find(c); i != std::wstring_view::npos) {
      return BopomofoKey{Kind::Consonant, static_cast<int>(i) + 1};
    }
    if (auto i = kMedialKeys.find(c); i != std::wstring_view::npos) {
      return BopomofoKey{Kind::Medial, static_cast<int>(i) + 1};
    }
    if (auto i = kVowelKeys.find(c); i != std::wstring_view::npos) {
      return BopomofoKey{Kind::Vowel, static_cast<int>(i) + 1};
    }
    switch (c) {
      case L' ': return BopomofoKey{Kind::Tone, 1};
      case L'6': return BopomofoKey{Kind::Tone, 2};
      case L'3': return BopomofoKey{Kind::Tone, 3};
      case L'4': return BopomofoKey{Kind::Tone, 4};
      case L'7': return BopomofoKey{Kind::Tone, 5};
      default: return std::nullopt;
    }
  }
};

}  // namespace

std::unique_ptr<BopomofoLayout> CreateStandardLayout() { return std::make_unique<StandardLayout>(); }

BopomofoReading::BopomofoReading(std::shared_ptr<const BopomofoLayout> layout)
    : layout_(std::move(layout)) {}

BopomofoReading::Result BopomofoReading::Feed(wchar_t usChar) {
  auto key = layout_->Map(usChar);
  if (!key) return Result::NotHandled;
  switch (key->kind) {
    case Kind::Consonant:
      current_.setConsonant(key->value);
      return Result::Updated;
    case Kind::Medial:
      current_.setMedial(key->value);
      return Result::Updated;
    case Kind::Vowel:
      current_.setVowel(key->value);
      return Result::Updated;
    case Kind::Tone:
      if (current_.empty()) return Result::Ignored;
      current_.setTone(key->value);
      return Result::Completed;
  }
  return Result::NotHandled;
}

Syllable BopomofoReading::TakeSyllable() {
  Syllable s = current_;
  current_ = Syllable();
  return s;
}

bool BopomofoReading::Backspace() {
  if (current_.vowel()) {
    current_.setVowel(0);
  } else if (current_.medial()) {
    current_.setMedial(0);
  } else if (current_.consonant()) {
    current_.setConsonant(0);
  } else {
    return false;
  }
  return true;
}

}  // namespace llaime
