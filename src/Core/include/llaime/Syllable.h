#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace llaime {

// A Bopomofo syllable packed into 16 bits (must match tools/build_lexicon.py):
//   bits 0-4   consonant  1..21  ㄅ..ㄙ
//   bits 5-6   medial     1..3   ㄧㄨㄩ
//   bits 7-10  vowel      1..13  ㄚ..ㄦ
//   bits 11-13 tone       1..5   (0 = unspecified, used for tone-less lookup)
class Syllable {
 public:
  static constexpr uint16_t kConsonantMask = 0x1f;
  static constexpr uint16_t kMedialMask = 0x3 << 5;
  static constexpr uint16_t kVowelMask = 0xf << 7;
  static constexpr int kToneShift = 11;
  static constexpr uint16_t kToneMask = 0x7 << kToneShift;

  constexpr Syllable() = default;
  constexpr explicit Syllable(uint16_t bits) : bits_(bits) {}

  constexpr uint16_t bits() const { return bits_; }
  constexpr int consonant() const { return bits_ & kConsonantMask; }
  constexpr int medial() const { return (bits_ & kMedialMask) >> 5; }
  constexpr int vowel() const { return (bits_ & kVowelMask) >> 7; }
  constexpr int tone() const { return (bits_ & kToneMask) >> kToneShift; }

  constexpr bool empty() const { return (bits_ & ~kToneMask) == 0; }
  constexpr bool hasTone() const { return tone() != 0; }
  constexpr Syllable withoutTone() const { return Syllable(bits_ & ~kToneMask); }

  constexpr void setConsonant(int v) { Set(kConsonantMask, v); }
  constexpr void setMedial(int v) { Set(kMedialMask, v << 5); }
  constexpr void setVowel(int v) { Set(kVowelMask, v << 7); }
  constexpr void setTone(int v) { Set(kToneMask, v << kToneShift); }

  // Display form, e.g. "ㄧˋ". Tone 1 is shown without a mark.
  std::wstring ToString() const;
  static std::optional<Syllable> Parse(std::wstring_view text);

  friend constexpr bool operator==(Syllable a, Syllable b) { return a.bits_ == b.bits_; }

 private:
  constexpr void Set(uint16_t mask, int shifted) {
    bits_ = static_cast<uint16_t>((bits_ & ~mask) | (shifted & mask));
  }

  uint16_t bits_ = 0;
};

// Symbol tables shared by the reading state machine and display code.
inline constexpr std::wstring_view kConsonants = L"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙ";
inline constexpr std::wstring_view kMedials = L"ㄧㄨㄩ";
inline constexpr std::wstring_view kVowels = L"ㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ";
// Index = tone number; tone 1 has no mark.
inline constexpr std::wstring_view kToneMarks = L"  ˊˇˋ˙";

}  // namespace llaime
