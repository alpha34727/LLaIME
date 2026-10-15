#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "llaime/Syllable.h"

namespace llaime {

struct LexEntry {
  std::wstring_view text;  // points into the mapped file
  float score;             // log10 unigram probability
};

// One read-only, memory-mapped lexicon file (layer) produced by tools/build_lexicon.py or
// WriteLexicon(). The mapping is shared between processes, so every app that loads the TIP
// pays for it only once.
class LexiconLayer {
 public:
  LexiconLayer() = default;
  ~LexiconLayer();
  LexiconLayer(const LexiconLayer&) = delete;
  LexiconLayer& operator=(const LexiconLayer&) = delete;

  bool Open(const std::wstring& path);
  // For tests: use an in-memory image instead of a file.
  bool OpenImage(std::vector<uint8_t> image);
  bool IsOpen() const { return base_ != nullptr; }

  size_t keyCount() const { return keys_.size(); }
  size_t entryCount() const { return entries_.size(); }

  bool Lookup(std::span<const Syllable> seq, std::vector<LexEntry>& out) const;
  bool LookupNamed(std::wstring_view name, std::vector<LexEntry>& out) const;
  // Every reading entry (not the named tables), in key order.
  void ForEach(const std::function<void(std::span<const uint16_t> reading, const LexEntry&)>& f)
      const;

 private:
#pragma pack(push, 1)
  struct Header {
    char magic[4];
    uint32_t version, keyCount, tonelessCount, namedCount, entryCount, syllableCount,
        stringCount;
  };
  struct Key {
    uint32_t offset;
    uint16_t length;
    uint16_t entryCount;
    uint32_t firstEntry;
  };
  struct Entry {
    uint32_t strOffset;
    uint16_t strLength;
    uint16_t reserved;
    float score;
  };
#pragma pack(pop)

  bool Parse(const uint8_t* data, size_t size);
  void Close();
  std::span<const uint16_t> KeySyllables(const Key& k) const;
  void AppendEntries(const Key& k, std::vector<LexEntry>& out) const;
  const Key* FindExact(std::span<const Syllable> seq) const;

  void* mapping_ = nullptr;  // HANDLE
  const uint8_t* base_ = nullptr;
  std::vector<uint8_t> image_;
  std::span<const Key> keys_;
  std::span<const uint32_t> toneless_;
  std::span<const Key> named_;
  std::span<const uint16_t> syllables_;
  std::span<const Entry> entries_;
  std::span<const wchar_t> strings_;
};

// The lexicon the composer converts with: one or more layers (the McBopomofo main table, the
// supplements, lexicons the user imported) looked up together. A text found in several layers
// is listed once, with its best score.
class Lexicon {
 public:
  Lexicon() = default;
  Lexicon(const Lexicon&) = delete;
  Lexicon& operator=(const Lexicon&) = delete;

  // A single file replaces any open layers. A directory opens every layer in it, in catalog
  // order (see LexiconCatalog.h).
  bool Open(const std::wstring& path);
  // Adds a layer after the open ones.
  bool AddLayer(const std::wstring& path);
  // For tests: a layer from an in-memory image (replaces any open layers).
  bool OpenImage(std::vector<uint8_t> image);
  bool AddImage(std::vector<uint8_t> image);
  void Clear() { layers_.clear(); }

  bool IsOpen() const { return !layers_.empty(); }
  size_t layerCount() const { return layers_.size(); }
  const LexiconLayer& layer(size_t i) const { return *layers_[i]; }

  static constexpr size_t kMaxPhraseLength = 9;

  // Appends entries whose reading matches `seq`, best first. Syllables without a tone match
  // any tone. Returns false if nothing matched.
  bool Lookup(std::span<const Syllable> seq, std::vector<LexEntry>& out) const;
  bool Contains(std::span<const Syllable> seq) const;
  // Named tables such as "_punctuation_<".
  bool LookupNamed(std::wstring_view name, std::vector<LexEntry>& out) const;

 private:
  std::vector<std::unique_ptr<LexiconLayer>> layers_;
};

// Sorts out[from..] best first and keeps each text once (its best score).
void MergeEntries(std::vector<LexEntry>& out, size_t from);

}  // namespace llaime
