#include "llaime/LexiconBuilder.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <numeric>

namespace llaime {
namespace {

constexpr wchar_t kFirstToneMark = 0x02C9;  // ˉ

void Append(std::vector<uint8_t>& out, const void* data, size_t bytes) {
  const auto* p = static_cast<const uint8_t*>(data);
  out.insert(out.end(), p, p + bytes);
}

template <typename T>
void Put(std::vector<uint8_t>& out, T value) {
  Append(out, &value, sizeof(value));
}

void Align4(std::vector<uint8_t>& out) {
  while (out.size() % 4) out.push_back(0);
}

bool IsBopomofo(wchar_t ch) {
  return kConsonants.find(ch) != std::wstring_view::npos ||
         kMedials.find(ch) != std::wstring_view::npos ||
         kVowels.find(ch) != std::wstring_view::npos ||
         (kToneMarks.find(ch) != std::wstring_view::npos && ch != L' ') || ch == kFirstToneMark;
}

bool IsReadingToken(std::wstring_view token) {
  if (token.empty()) return false;
  bool any = false;
  for (wchar_t ch : token) {
    if (ch == L'-') continue;
    if (!IsBopomofo(ch)) return false;
    any = true;
  }
  return any;
}

bool IsNumberToken(std::wstring_view token, double* value) {
  if (token.empty()) return false;
  std::wstring s(token);
  wchar_t* end = nullptr;
  *value = wcstod(s.c_str(), &end);
  return end && *end == L'\0';
}

std::wstring_view Trim(std::wstring_view s) {
  while (!s.empty() && std::iswspace(s.front())) s.remove_prefix(1);
  while (!s.empty() && std::iswspace(s.back())) s.remove_suffix(1);
  return s;
}

}  // namespace

float UnattestedScore(size_t length) {
  return static_cast<float>(-8.3353 + 0.43136 * static_cast<double>(length));
}

void LexiconBuilder::Add(std::span<const Syllable> reading, std::wstring_view text, float score) {
  if (reading.empty() || text.empty()) return;
  std::vector<uint16_t> key;
  for (Syllable s : reading) key.push_back(s.bits());
  auto& values = keys_[key];
  auto [it, inserted] = values.try_emplace(std::wstring(text), score);
  if (!inserted) it->second = std::max(it->second, score);
}

size_t LexiconBuilder::size() const {
  size_t n = 0;
  for (const auto& [key, values] : keys_) n += values.size();
  return n;
}

// Mirrors tools/build_lexicon.py; see the layout described there.
std::vector<uint8_t> LexiconBuilder::Build() const {
  std::vector<uint16_t> strings;
  std::unordered_map<std::wstring, uint32_t> stringOffsets;
  auto intern = [&](const std::wstring& s) {
    auto [it, inserted] = stringOffsets.try_emplace(s, static_cast<uint32_t>(strings.size()));
    if (inserted) strings.insert(strings.end(), s.begin(), s.end());
    return it->second;
  };

  std::vector<uint8_t> keys, entries;
  std::vector<uint16_t> syllables;
  std::vector<std::vector<uint16_t>> sequences;
  uint32_t entryCount = 0;
  for (const auto& [seq, values] : keys_) {
    std::vector<std::pair<std::wstring, float>> sorted(values.begin(), values.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    const uint32_t first = entryCount;
    for (const auto& [text, score] : sorted) {
      Put<uint32_t>(entries, intern(text));
      Put<uint16_t>(entries, static_cast<uint16_t>(text.size()));
      Put<uint16_t>(entries, 0);
      Put<float>(entries, score);
      ++entryCount;
    }
    Put<uint32_t>(keys, static_cast<uint32_t>(syllables.size()));
    Put<uint16_t>(keys, static_cast<uint16_t>(seq.size()));
    Put<uint16_t>(keys, static_cast<uint16_t>(entryCount - first));
    Put<uint32_t>(keys, first);
    syllables.insert(syllables.end(), seq.begin(), seq.end());
    sequences.push_back(seq);
  }

  std::vector<uint32_t> toneless(sequences.size());
  std::iota(toneless.begin(), toneless.end(), 0u);
  auto stripped = [&](uint32_t i) {
    std::vector<uint16_t> s = sequences[i];
    for (auto& x : s) x &= static_cast<uint16_t>(~Syllable::kToneMask);
    return s;
  };
  std::stable_sort(toneless.begin(), toneless.end(),
                   [&](uint32_t a, uint32_t b) { return stripped(a) < stripped(b); });

  std::vector<uint8_t> out;
  Append(out, "LLXL", 4);
  for (uint32_t v : {1u, static_cast<uint32_t>(sequences.size()),
                     static_cast<uint32_t>(toneless.size()), 0u, entryCount,
                     static_cast<uint32_t>(syllables.size()),
                     static_cast<uint32_t>(strings.size())}) {
    Put(out, v);
  }
  Append(out, keys.data(), keys.size());
  Align4(out);
  Append(out, toneless.data(), toneless.size() * sizeof(uint32_t));
  Align4(out);
  // No named tables.
  Append(out, syllables.data(), syllables.size() * sizeof(uint16_t));
  Align4(out);
  Append(out, entries.data(), entries.size());
  Align4(out);
  Append(out, strings.data(), strings.size() * sizeof(uint16_t));
  Align4(out);
  return out;
}

bool LexiconBuilder::Write(const std::wstring& path) const {
  const std::vector<uint8_t> image = Build();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, image.data(), static_cast<DWORD>(image.size()), &written,
                            nullptr) &&
                  written == image.size();
  CloseHandle(file);
  return ok;
}

std::vector<std::wstring_view> CodePoints(std::wstring_view text) {
  std::vector<std::wstring_view> out;
  for (size_t i = 0; i < text.size();) {
    const size_t n = IS_HIGH_SURROGATE(text[i]) && i + 1 < text.size() ? 2 : 1;
    out.push_back(text.substr(i, n));
    i += n;
  }
  return out;
}

ReadingGuesser::ReadingGuesser(const Lexicon& lexicon) {
  for (size_t i = 0; i < lexicon.layerCount(); ++i) {
    lexicon.layer(i).ForEach([&](std::span<const uint16_t> reading, const LexEntry& entry) {
      auto& readings = index_[std::wstring(entry.text)];
      std::vector<uint16_t> syllables(reading.begin(), reading.end());
      for (auto& r : readings) {
        if (r.syllables == syllables) {
          r.score = std::max(r.score, entry.score);
          return;
        }
      }
      readings.push_back({std::move(syllables), entry.score});
    });
  }
  for (auto& [text, readings] : index_) {
    std::stable_sort(readings.begin(), readings.end(),
                     [](const Reading& a, const Reading& b) { return a.score > b.score; });
  }
}

const std::vector<ReadingGuesser::Reading>* ReadingGuesser::Find(std::wstring_view text) const {
  auto it = index_.find(std::wstring(text));
  return it == index_.end() ? nullptr : &it->second;
}

std::vector<std::vector<Syllable>> ReadingGuesser::Guess(std::wstring_view text,
                                                         size_t max) const {
  std::vector<std::vector<Syllable>> out;
  auto add = [&](const std::vector<uint16_t>& bits) {
    std::vector<Syllable> reading;
    for (uint16_t b : bits) reading.push_back(Syllable(b));
    if (out.size() < max && std::find(out.begin(), out.end(), reading) == out.end()) {
      out.push_back(std::move(reading));
    }
  };
  const auto chars = CodePoints(text);
  if (chars.empty()) return out;

  // The phrase as the lexicon lists it.
  if (const auto* whole = Find(text)) {
    for (const auto& r : *whole) {
      if (r.syllables.size() == chars.size()) add(r.syllables);
    }
  }

  // Best split into known phrases (Viterbi on log10 scores; fewer, likelier pieces win).
  const size_t n = chars.size();
  std::vector<size_t> offsets(n + 1, 0);
  for (size_t i = 0; i < n; ++i) offsets[i + 1] = offsets[i] + chars[i].size();
  constexpr float kNone = -1e30f;
  std::vector<float> best(n + 1, kNone);
  std::vector<size_t> from(n + 1, 0);
  std::vector<const Reading*> piece(n + 1, nullptr);
  best[0] = 0;
  for (size_t end = 1; end <= n; ++end) {
    for (size_t len = 1; len <= std::min(end, Lexicon::kMaxPhraseLength); ++len) {
      const size_t start = end - len;
      if (best[start] == kNone) continue;
      const auto* readings =
          Find(text.substr(offsets[start], offsets[end] - offsets[start]));
      if (!readings) continue;
      for (const auto& r : *readings) {
        if (r.syllables.size() != len) continue;
        if (best[start] + r.score > best[end]) {
          best[end] = best[start] + r.score;
          from[end] = start;
          piece[end] = &r;
        }
        break;  // readings are sorted: the first of this length is the best
      }
    }
  }
  if (best[n] == kNone) return out;
  std::vector<std::pair<size_t, const Reading*>> pieces;  // (start, reading)
  for (size_t end = n; end > 0; end = from[end]) pieces.emplace_back(from[end], piece[end]);
  std::reverse(pieces.begin(), pieces.end());
  std::vector<uint16_t> split;
  for (const auto& [start, r] : pieces) split.insert(split.end(), r->syllables.begin(),
                                                     r->syllables.end());
  add(split);

  // One single-character piece read another way.
  for (const auto& [start, r] : pieces) {
    if (r->syllables.size() != 1) continue;
    for (const auto& other : *Find(chars[start])) {
      if (other.syllables.size() != 1 || other.syllables == r->syllables) continue;
      std::vector<uint16_t> variant = split;
      variant[start] = other.syllables[0];
      add(variant);
    }
  }
  return out;
}

std::optional<std::vector<Syllable>> ParseWrittenReading(std::wstring_view text) {
  std::vector<Syllable> out;
  std::wstring part;
  auto flush = [&]() {
    if (part.empty()) return true;
    std::wstring_view p = part;
    if (p.back() == kFirstToneMark) p.remove_suffix(1);
    auto s = Syllable::Parse(p);  // no mark: tone 1
    part.clear();
    if (!s) return false;
    out.push_back(*s);
    return true;
  };
  for (wchar_t ch : text) {
    if (ch == L'-' || std::iswspace(ch)) {
      if (!flush()) return std::nullopt;
    } else {
      part += ch;
    }
  }
  if (!flush() || out.empty()) return std::nullopt;
  return out;
}

std::wstring DisplayReading(std::span<const Syllable> reading) {
  std::wstring out;
  for (size_t i = 0; i < reading.size(); ++i) {
    if (i) out += L' ';
    out += reading[i].ToString();
  }
  return out;
}

std::optional<ImportedPhrase> ParseLexiconLine(std::wstring_view line) {
  line = Trim(line);
  if (line.empty() || line.front() == L'#') return std::nullopt;
  // Fields are separated by tabs, commas or spaces; a run of reading syllables separated by
  // spaces (tsi.src, tsi.csv) is one reading.
  std::vector<std::wstring_view> tokens;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (std::iswspace(line[i]) || line[i] == L',')) ++i;
    const size_t start = i;
    while (i < line.size() && !std::iswspace(line[i]) && line[i] != L',') ++i;
    if (i > start) tokens.push_back(line.substr(start, i - start));
  }
  ImportedPhrase phrase;
  std::wstring reading;
  for (std::wstring_view token : tokens) {
    double number = 0;
    if (IsReadingToken(token)) {
      if (!reading.empty()) reading += L' ';
      reading += token;
    } else if (IsNumberToken(token, &number)) {
      // Negative numbers are log10 scores (McBopomofo); counts (libchewing) are not on that
      // scale and are left out.
      if (number < 0 && !phrase.score) phrase.score = static_cast<float>(number);
    } else if (phrase.text.empty()) {
      phrase.text = token;
    }
  }
  if (phrase.text.empty()) return std::nullopt;
  if (!reading.empty()) {
    auto parsed = ParseWrittenReading(reading);
    if (parsed) phrase.reading = std::move(*parsed);
  }
  return phrase;
}

}  // namespace llaime
