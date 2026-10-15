#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "llaime/Lexicon.h"
#include "llaime/Syllable.h"

namespace llaime {

// McBopomofo's score (log10) for a phrase without frequency data, by length in characters
// (tools/supplement_lexicon.py unattested_score).
float UnattestedScore(size_t length);

// Builds a lexicon layer in the format of tools/build_lexicon.py (reading tables only).
class LexiconBuilder {
 public:
  // Every syllable needs a tone (tone 1 = 1). A text repeated under a reading keeps its best
  // score.
  void Add(std::span<const Syllable> reading, std::wstring_view text, float score);
  size_t size() const;
  std::vector<uint8_t> Build() const;
  bool Write(const std::wstring& path) const;

 private:
  std::map<std::vector<uint16_t>, std::map<std::wstring, float>> keys_;
};

// Readings of a text from the lexicon, for words people add or import without one.
class ReadingGuesser {
 public:
  // Indexes every reading entry of every layer of `lexicon` (which must outlive this).
  explicit ReadingGuesser(const Lexicon& lexicon);

  // Up to `max` readings for `text`, most likely first: the readings the lexicon lists for
  // the whole phrase, then the best split into known phrases and characters, then that split
  // with one character read another way (破音字). Empty if a character has no known reading.
  std::vector<std::vector<Syllable>> Guess(std::wstring_view text, size_t max = 6) const;

 private:
  struct Reading {
    std::vector<uint16_t> syllables;
    float score;
  };
  const std::vector<Reading>* Find(std::wstring_view text) const;

  std::unordered_map<std::wstring, std::vector<Reading>> index_;
};

// Splits text into Unicode code points (as UTF-16 pieces), one per syllable.
std::vector<std::wstring_view> CodePoints(std::wstring_view text);

// A reading as people write it: syllables separated by spaces or '-', tone 1 with no mark (or
// ˉ). Every syllable gets a tone. nullopt if a syllable is not Bopomofo.
std::optional<std::vector<Syllable>> ParseWrittenReading(std::wstring_view text);
// Syllables separated by spaces, tone 1 unmarked (toneless syllables are shown the same).
std::wstring DisplayReading(std::span<const Syllable> reading);

// One line of a lexicon or word list file, as understood by ParseLexiconLine.
struct ImportedPhrase {
  std::wstring text;
  std::vector<Syllable> reading;  // empty: none given
  std::optional<float> score;     // log10, when the line has one
};

// Reads a line of any of the common formats, telling the text, its reading and a score apart
// by what they contain:
//   McBopomofo data      ㄅㄚˋ-ㄅㄚ˙ 爸爸 -5.21
//   McBopomofo user      爸爸 ㄅㄚˋ-ㄅㄚ˙
//   libchewing tsi.csv   爸爸,1234,ㄅㄚˋ ㄅㄚ˙
//   libchewing tsi.src   爸爸 1234 ㄅㄚˋ ㄅㄚ˙
//   word list            爸爸
// Blank lines and # comments give nullopt, as do lines without a text.
std::optional<ImportedPhrase> ParseLexiconLine(std::wstring_view line);

}  // namespace llaime
