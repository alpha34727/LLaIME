#include "llaime/UserDictionary.h"

#include <algorithm>
#include <limits>

namespace llaime {

std::wstring FormatReading(std::span<const Syllable> reading) {
  std::wstring out;
  for (size_t i = 0; i < reading.size(); ++i) {
    if (i) out += L'-';
    std::wstring s = reading[i].ToString();
    // Syllable::ToString shows tone 1 and "no tone" alike; keep tone 1 explicit so the stored
    // reading round-trips (ˉ is the first-tone mark).
    if (reading[i].tone() == 1) s += static_cast<wchar_t>(0x02C9);
    out += s;
  }
  return out;
}

std::optional<std::vector<Syllable>> ParseReading(std::wstring_view text) {
  std::vector<Syllable> out;
  while (!text.empty()) {
    const size_t dash = text.find(L'-');
    std::wstring_view part = text.substr(0, dash);
    bool firstTone = false, toneless = true;
    if (!part.empty() && part.back() == static_cast<wchar_t>(0x02C9)) {
      firstTone = true;
      part.remove_suffix(1);
    }
    for (wchar_t ch : part) {
      if (kToneMarks.find(ch) != std::wstring_view::npos && ch != L' ') toneless = false;
    }
    auto s = Syllable::Parse(part);
    if (!s) return std::nullopt;
    // Parse() gives a mark-less syllable tone 1; without ˉ it was typed without a tone.
    if (toneless && !firstTone) s->setTone(0);
    out.push_back(*s);
    if (dash == std::wstring_view::npos) break;
    text.remove_prefix(dash + 1);
  }
  if (out.empty() || out.size() > Lexicon::kMaxPhraseLength) return std::nullopt;
  return out;
}

std::u16string UserDictionary::TonelessKey(std::span<const Syllable> reading) {
  std::u16string key;
  for (Syllable s : reading) key += static_cast<char16_t>(s.withoutTone().bits());
  return key;
}

bool UserDictionary::ReadingMatches(std::span<const Syllable> stored,
                                    std::span<const Syllable> typed) {
  if (stored.size() != typed.size()) return false;
  for (size_t i = 0; i < stored.size(); ++i) {
    if (stored[i].withoutTone() != typed[i].withoutTone()) return false;
    if (stored[i].hasTone() && typed[i].hasTone() && stored[i].tone() != typed[i].tone()) {
      return false;
    }
  }
  return true;
}

void UserDictionary::AddPhrase(std::vector<Syllable> reading, std::wstring text, PhraseKind kind) {
  if (reading.empty() || reading.size() > Lexicon::kMaxPhraseLength || text.empty()) return;
  phraseIndex_[TonelessKey(reading)].push_back(static_cast<uint32_t>(phrases_.size()));
  phrases_.push_back({std::move(reading), std::move(text), kind});
}

void UserDictionary::AddSelection(std::vector<Syllable> reading, std::wstring text,
                                  std::wstring prev, double weight) {
  if (reading.empty() || reading.size() > Lexicon::kMaxPhraseLength || text.empty() ||
      weight < kMinWeight) {
    return;
  }
  selectionIndex_[TonelessKey(reading)].push_back(static_cast<uint32_t>(selections_.size()));
  selections_.push_back({std::move(reading), std::move(text), std::move(prev), weight});
}

bool UserDictionary::Lookup(const Lexicon* lexicon, std::span<const Syllable> seq,
                            std::vector<LexEntry>& out) const {
  out.clear();
  if (lexicon) lexicon->Lookup(seq, out);
  if (phrases_.empty() && selections_.empty()) return !out.empty();
  const std::u16string key = TonelessKey(seq);

  float top = -std::numeric_limits<float>::infinity();
  for (const LexEntry& e : out) top = std::max(top, e.score);
  if (auto it = phraseIndex_.find(key); it != phraseIndex_.end()) {
    for (uint32_t i : it->second) {
      const Phrase& p = phrases_[i];
      if (ReadingMatches(p.reading, seq)) out.push_back({p.text, kPhraseScore});
    }
  }
  if (auto it = selectionIndex_.find(key); it != selectionIndex_.end()) {
    // Picked words, most weight first, each placed above the reading's best entry.
    std::vector<const Selection*> picked;
    for (uint32_t i : it->second) {
      const Selection& s = selections_[i];
      if (s.prev.empty() && ReadingMatches(s.reading, seq)) picked.push_back(&s);
    }
    std::stable_sort(picked.begin(), picked.end(),
                     [](const Selection* a, const Selection* b) { return a->weight > b->weight; });
    for (const LexEntry& e : out) top = std::max(top, e.score);
    for (size_t rank = 0; rank < picked.size(); ++rank) {
      // Only words still offered (a lexicon layer may have been turned off since).
      auto entry = std::find_if(out.begin(), out.end(),
                                [&](const LexEntry& e) { return e.text == picked[rank]->text; });
      if (entry == out.end()) continue;
      const float boosted = top + kSelectionMargin * static_cast<float>(picked.size() - rank);
      entry->score = std::max(entry->score, boosted);
    }
  }
  // Best first; a text found twice (lexicon and user phrase) keeps its best score.
  std::stable_sort(out.begin(), out.end(),
                   [](const LexEntry& a, const LexEntry& b) { return a.score > b.score; });
  auto end = out.end();
  for (auto it = out.begin(); it != end; ++it) {
    end = std::remove_if(it + 1, end, [&](const LexEntry& e) { return e.text == it->text; });
  }
  out.erase(end, out.end());
  return !out.empty();
}

double UserDictionary::SelectionWeight(std::span<const Syllable> reading, std::wstring_view text,
                                       std::wstring_view prev) const {
  auto it = selectionIndex_.find(TonelessKey(reading));
  if (it == selectionIndex_.end()) return 0;
  double weight = 0;
  for (uint32_t i : it->second) {
    const Selection& s = selections_[i];
    if (s.text == text && s.prev == prev && ReadingMatches(s.reading, reading)) {
      weight = std::max(weight, s.weight);
    }
  }
  return weight;
}

bool UserDictionary::ValidShortcut(std::wstring_view keys) {
  return !keys.empty() && keys.size() <= kMaxShortcutLength &&
         std::all_of(keys.begin(), keys.end(), [](wchar_t c) {
           return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
         });
}

void UserDictionary::AddShortcut(std::wstring keys, std::wstring text) {
  if (!ValidShortcut(keys) || text.empty()) return;
  shortcuts_[std::move(keys)] = std::move(text);
}

UserDictionary::ShortcutMatch UserDictionary::MatchShortcut(std::wstring_view keys) const {
  ShortcutMatch match;
  // Shortcuts starting with `keys` follow it in sorted order.
  for (auto it = shortcuts_.lower_bound(keys);
       it != shortcuts_.end() && it->first.starts_with(keys); ++it) {
    if (it->first.size() == keys.size()) {
      match.text = &it->second;
    } else {
      match.longer = true;
      break;
    }
  }
  return match;
}

bool UserDictionary::HasPhrase(std::span<const Syllable> reading, std::wstring_view text) const {
  auto it = phraseIndex_.find(TonelessKey(reading));
  if (it == phraseIndex_.end()) return false;
  return std::any_of(it->second.begin(), it->second.end(), [&](uint32_t i) {
    return phrases_[i].text == text && ReadingMatches(phrases_[i].reading, reading);
  });
}

}  // namespace llaime
