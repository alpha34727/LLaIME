#include "llaime/Syllable.h"

namespace llaime {

std::wstring Syllable::ToString() const {
  std::wstring s;
  if (consonant()) s += kConsonants[consonant() - 1];
  if (medial()) s += kMedials[medial() - 1];
  if (vowel()) s += kVowels[vowel() - 1];
  if (tone() >= 2) s += kToneMarks[tone()];
  return s;
}

std::optional<Syllable> Syllable::Parse(std::wstring_view text) {
  Syllable s;
  int tone = 1;
  for (wchar_t ch : text) {
    if (auto i = kConsonants.find(ch); i != std::wstring_view::npos) {
      s.setConsonant(static_cast<int>(i) + 1);
    } else if (auto j = kMedials.find(ch); j != std::wstring_view::npos) {
      s.setMedial(static_cast<int>(j) + 1);
    } else if (auto k = kVowels.find(ch); k != std::wstring_view::npos) {
      s.setVowel(static_cast<int>(k) + 1);
    } else if (auto t = kToneMarks.find(ch); t != std::wstring_view::npos && t >= 2) {
      tone = static_cast<int>(t);
    } else {
      return std::nullopt;
    }
  }
  if (s.empty()) return std::nullopt;
  s.setTone(tone);
  return s;
}

}  // namespace llaime
