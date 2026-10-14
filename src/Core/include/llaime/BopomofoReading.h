#pragma once
#include <memory>
#include <optional>
#include <string>

#include "llaime/Syllable.h"

namespace llaime {

// What a physical key means in a Bopomofo keyboard layout.
struct BopomofoKey {
  enum class Kind { Consonant, Medial, Vowel, Tone } kind;
  int value;  // 1-based index into kConsonants/kMedials/kVowels, or tone number 1..5
};

// Maps an unshifted US-keyboard character (e.g. '1', 'q', ',', ' ') to a Bopomofo key.
// Implementations: StandardLayout (大千). ETen, Hsu, ... can be added later.
class BopomofoLayout {
 public:
  virtual ~BopomofoLayout() = default;
  virtual std::optional<BopomofoKey> Map(wchar_t usChar) const = 0;
};

std::unique_ptr<BopomofoLayout> CreateStandardLayout();

// The syllable being typed (before the tone key commits it).
class BopomofoReading {
 public:
  explicit BopomofoReading(std::shared_ptr<const BopomofoLayout> layout);

  enum class Result {
    NotHandled,  // key is not part of the layout
    Updated,     // reading changed
    Completed,   // a tone key finished the syllable; read it with TakeSyllable()
    Ignored,     // layout key that does nothing here (tone on an empty reading)
  };

  Result Feed(wchar_t usChar);
  bool IsLayoutKey(wchar_t usChar) const { return layout_->Map(usChar).has_value(); }
  bool IsToneKey(wchar_t usChar) const {
    auto key = layout_->Map(usChar);
    return key && key->kind == BopomofoKey::Kind::Tone;
  }

  Syllable TakeSyllable();
  Syllable Current() const { return current_; }
  bool Empty() const { return current_.empty(); }
  void Clear() { current_ = Syllable(); }
  void Restore(Syllable s) { current_ = s; }
  bool Backspace();  // removes the last-typed component (and any tone left by Restore)
  std::wstring Text() const { return current_.ToString(); }

 private:
  std::shared_ptr<const BopomofoLayout> layout_;
  Syllable current_;
};

}  // namespace llaime
