#include "llaime/Lexicon.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace llaime {
namespace {

size_t Align4(size_t n) { return (n + 3) & ~size_t{3}; }

// Three-way compare of syllable sequences, optionally ignoring tones.
int CompareSeq(std::span<const uint16_t> a, std::span<const Syllable> b, bool ignoreTone) {
  const uint16_t mask = ignoreTone ? static_cast<uint16_t>(~Syllable::kToneMask) : 0xffff;
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    uint16_t x = a[i] & mask;
    uint16_t y = b[i].bits() & mask;
    if (x != y) return x < y ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

}  // namespace

Lexicon::~Lexicon() { Close(); }

void Lexicon::Close() {
  if (mapping_) {
    UnmapViewOfFile(base_);
    CloseHandle(mapping_);
    mapping_ = nullptr;
  }
  base_ = nullptr;
  image_.clear();
}

bool Lexicon::Open(const std::wstring& path) {
  Close();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  GetFileSizeEx(file, &size);
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  CloseHandle(file);
  if (!mapping) return false;
  auto* view = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
  if (!view) {
    CloseHandle(mapping);
    return false;
  }
  mapping_ = mapping;
  base_ = view;
  if (!Parse(view, static_cast<size_t>(size.QuadPart))) {
    Close();
    return false;
  }
  return true;
}

bool Lexicon::OpenImage(std::vector<uint8_t> image) {
  Close();
  image_ = std::move(image);
  base_ = image_.data();
  if (!Parse(image_.data(), image_.size())) {
    Close();
    return false;
  }
  return true;
}

bool Lexicon::Parse(const uint8_t* data, size_t size) {
  if (size < sizeof(Header)) return false;
  Header h;
  std::memcpy(&h, data, sizeof(h));
  if (std::memcmp(h.magic, "LLXL", 4) != 0 || h.version != 1) return false;

  size_t offset = sizeof(Header);
  auto take = [&](size_t bytes) -> const uint8_t* {
    if (offset + bytes > size) return nullptr;
    const uint8_t* p = data + offset;
    offset = Align4(offset + bytes);
    return p;
  };
  auto* keys = take(h.keyCount * sizeof(Key));
  auto* toneless = take(h.tonelessCount * sizeof(uint32_t));
  auto* named = take(h.namedCount * sizeof(Key));
  auto* syllables = take(h.syllableCount * sizeof(uint16_t));
  auto* entries = take(h.entryCount * sizeof(Entry));
  auto* strings = take(h.stringCount * sizeof(wchar_t));
  if (!keys || !toneless || !named || !syllables || !entries || !strings) return false;

  keys_ = {reinterpret_cast<const Key*>(keys), h.keyCount};
  toneless_ = {reinterpret_cast<const uint32_t*>(toneless), h.tonelessCount};
  named_ = {reinterpret_cast<const Key*>(named), h.namedCount};
  syllables_ = {reinterpret_cast<const uint16_t*>(syllables), h.syllableCount};
  entries_ = {reinterpret_cast<const Entry*>(entries), h.entryCount};
  strings_ = {reinterpret_cast<const wchar_t*>(strings), h.stringCount};
  return true;
}

std::span<const uint16_t> Lexicon::KeySyllables(const Key& k) const {
  return syllables_.subspan(k.offset, k.length);
}

void Lexicon::AppendEntries(const Key& k, std::vector<LexEntry>& out) const {
  for (uint32_t i = 0; i < k.entryCount; ++i) {
    const Entry& e = entries_[k.firstEntry + i];
    out.push_back({std::wstring_view(strings_.data() + e.strOffset, e.strLength), e.score});
  }
}

const Lexicon::Key* Lexicon::FindExact(std::span<const Syllable> seq) const {
  auto it = std::lower_bound(keys_.begin(), keys_.end(), seq, [&](const Key& k, auto s) {
    return CompareSeq(KeySyllables(k), s, false) < 0;
  });
  if (it != keys_.end() && CompareSeq(KeySyllables(*it), seq, false) == 0) return &*it;
  return nullptr;
}

bool Lexicon::Lookup(std::span<const Syllable> seq, std::vector<LexEntry>& out) const {
  if (!base_ || seq.empty() || seq.size() > kMaxPhraseLength) return false;
  bool allToned = std::all_of(seq.begin(), seq.end(), [](Syllable s) { return s.hasTone(); });
  if (allToned) {
    const Key* k = FindExact(seq);
    if (!k) return false;
    AppendEntries(*k, out);
    return true;
  }

  // Tone-less lookup: scan the range of keys equal to `seq` with tones stripped, then keep
  // those whose explicitly toned syllables match.
  auto lessThan = [&](uint32_t idx, auto s) {
    return CompareSeq(KeySyllables(keys_[idx]), s, true) < 0;
  };
  auto first = std::lower_bound(toneless_.begin(), toneless_.end(), seq, lessThan);
  size_t before = out.size();
  for (auto it = first; it != toneless_.end(); ++it) {
    const Key& k = keys_[*it];
    auto ks = KeySyllables(k);
    if (CompareSeq(ks, seq, true) != 0) break;
    bool match = true;
    for (size_t i = 0; i < seq.size(); ++i) {
      if (seq[i].hasTone() && ks[i] != seq[i].bits()) {
        match = false;
        break;
      }
    }
    if (match) AppendEntries(k, out);
  }
  if (out.size() == before) return false;
  // Merge results from different tones: best score first, drop duplicate texts.
  std::stable_sort(out.begin() + before, out.end(),
                   [](const LexEntry& a, const LexEntry& b) { return a.score > b.score; });
  auto end = out.end();
  for (auto it = out.begin() + before; it != end; ++it) {
    end = std::remove_if(it + 1, end, [&](const LexEntry& e) { return e.text == it->text; });
  }
  out.erase(end, out.end());
  return true;
}

bool Lexicon::Contains(std::span<const Syllable> seq) const {
  std::vector<LexEntry> tmp;
  return Lookup(seq, tmp);
}

bool Lexicon::LookupNamed(std::wstring_view name, std::vector<LexEntry>& out) const {
  if (!base_) return false;
  auto keyName = [&](const Key& k) {
    return std::wstring_view(strings_.data() + k.offset, k.length);
  };
  auto it = std::lower_bound(named_.begin(), named_.end(), name,
                             [&](const Key& k, std::wstring_view n) { return keyName(k) < n; });
  if (it == named_.end() || keyName(*it) != name) return false;
  AppendEntries(*it, out);
  return true;
}

}  // namespace llaime
