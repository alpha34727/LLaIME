#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <map>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "llaime/Lexicon.h"
#include "llaime/Syllable.h"

namespace llaime {

// Readings as stored in user.db and shown to people: syllables joined by '-', e.g.
// "ㄌㄧˇ-ㄒㄧㄠˇ-ㄇㄧㄥˊ" (McBopomofo style). A syllable typed without a tone is stored without
// one and matches any tone.
std::wstring FormatReading(std::span<const Syllable> reading);
std::optional<std::vector<Syllable>> ParseReading(std::wstring_view text);

// A snapshot of the user's own words and of what they learned, built by LLaIME.Engine from
// user.db and held by every TIP:
//  - phrases: added by hand (Ctrl+=, settings) or formed from characters picked one by one
//    again and again; they convert like lexicon phrases, ahead of them;
//  - selections: candidates the user picked, each with a weight that decays with time, overall
//    and after a given preceding word. A picked word moves to the front of its reading and is
//    favoured when the language model ranks whole sentences.
class UserDictionary {
 public:
  enum class PhraseKind : uint8_t { Manual = 0, Learned = 1 };

  // Lexicon-style score (log10) of a user phrase: above any lexicon entry, as McBopomofo does.
  static constexpr float kPhraseScore = 0.0f;
  // A picked word is placed this far (log10) above the best entry of its reading, enough to
  // lead the lexicon conversion without changing how the sentence is segmented.
  static constexpr float kSelectionMargin = 0.05f;
  // Weights below this are treated as forgotten.
  static constexpr double kMinWeight = 0.05;

  void AddPhrase(std::vector<Syllable> reading, std::wstring text, PhraseKind kind);
  // `prev` is the word before it ("" for the unconditional record).
  void AddSelection(std::vector<Syllable> reading, std::wstring text, std::wstring prev,
                    double weight);

  // A phrase typed by its shortcut keys (lower-case letters and digits).
  void AddShortcut(std::wstring keys, std::wstring text);

  bool empty() const { return phrases_.empty() && selections_.empty() && shortcuts_.empty(); }
  size_t phraseCount() const { return phrases_.size(); }
  size_t selectionCount() const { return selections_.size(); }

  // Lexicon entries for `seq` merged with the user's: user phrases and picked words first,
  // then the rest by score, each text once. `out` is replaced. Returns false if nothing
  // matched. Views point into the lexicon or this dictionary.
  bool Lookup(const Lexicon* lexicon, std::span<const Syllable> seq,
              std::vector<LexEntry>& out) const;

  // Weight of the user having picked `text` for `reading`: overall (prev empty) or after
  // `prev`. 0 if never picked.
  double SelectionWeight(std::span<const Syllable> reading, std::wstring_view text,
                         std::wstring_view prev = {}) const;

  // Whether `text` is a user phrase for `reading`.
  bool HasPhrase(std::span<const Syllable> reading, std::wstring_view text) const;

  // Shortcuts typed so far: the phrase of exactly `keys` (if any), whether a longer shortcut
  // starts with `keys`, and whether any does (exact or longer).
  struct ShortcutMatch {
    const std::wstring* text = nullptr;
    bool longer = false;
    bool any() const { return text || longer; }
  };
  ShortcutMatch MatchShortcut(std::wstring_view keys) const;
  static bool ValidShortcut(std::wstring_view keys);
  static constexpr size_t kMaxShortcutLength = 16;

 private:
  struct Phrase {
    std::vector<Syllable> reading;
    std::wstring text;
    PhraseKind kind;
  };
  struct Selection {
    std::vector<Syllable> reading;
    std::wstring text;
    std::wstring prev;
    double weight;
  };

  // Records are found by their reading with tones removed, then matched syllable by syllable
  // (a missing tone on either side matches any tone).
  static std::u16string TonelessKey(std::span<const Syllable> reading);
  static bool ReadingMatches(std::span<const Syllable> stored, std::span<const Syllable> typed);

  std::vector<Phrase> phrases_;
  std::vector<Selection> selections_;
  std::map<std::wstring, std::wstring, std::less<>> shortcuts_;  // keys -> text, sorted
  std::unordered_map<std::u16string, std::vector<uint32_t>> phraseIndex_;
  std::unordered_map<std::u16string, std::vector<uint32_t>> selectionIndex_;
};

}  // namespace llaime
