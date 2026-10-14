#include "LayaTokenizer.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace llaime {
namespace {

constexpr char32_t kMetaspace = 0x2581;  // ▁
constexpr uint32_t kMissing = 0xFFFFFFFF;

std::u32string DecodeUtf8(const std::string& s) {
  std::u32string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    char32_t cp;
    size_t len;
    if (c < 0x80) {
      cp = c;
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      len = 3;
    } else {
      cp = c & 0x07;
      len = 4;
    }
    if (i + len > s.size()) break;
    for (size_t k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += len;
  }
  return out;
}

void AppendUtf8(char32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

template <typename T>
bool Read(std::ifstream& in, T* value, size_t count = 1) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(value), sizeof(T) * count));
}

}  // namespace

bool LayaTokenizer::Load(const std::wstring& path, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  char magic[4];
  uint32_t header[11];
  if (!in || !Read(in, magic, 4) || std::memcmp(magic, "LLTK", 4) != 0 || !Read(in, header, 11) ||
      header[0] != 1) {
    if (error) *error = "not a tokenizer.bin (version 1)";
    return false;
  }
  const uint32_t charCount = header[2], mergeCount = header[3];
  special_ = {header[4], header[5], header[6], header[7], header[8]};
  maxLen_ = header[9];
  headMaxLen_ = header[10];
  chars_.resize(charCount);
  std::vector<uint32_t> merges(static_cast<size_t>(mergeCount) * 3);
  uint32_t instructionLength = 0;
  if (!Read(in, byteTokens_, 256) || !Read(in, chars_.data(), charCount) ||
      !Read(in, merges.data(), merges.size()) || !Read(in, &instructionLength)) {
    if (error) *error = "truncated tokenizer.bin";
    return false;
  }
  instruction_.resize(instructionLength);
  if (!in.read(instruction_.data(), instructionLength)) {
    if (error) *error = "truncated tokenizer.bin";
    return false;
  }
  merges_.resize(mergeCount);
  for (uint32_t r = 0; r < mergeCount; ++r) {
    merges_[r] = {static_cast<uint64_t>(merges[r * 3]) << 32 | merges[r * 3 + 1], r,
                  merges[r * 3 + 2]};
  }
  // A pair can appear more than once; the first (lowest-rank) merge wins, as in tokenizers.
  std::stable_sort(merges_.begin(), merges_.end(),
                   [](const Merge& a, const Merge& b) { return a.pair < b.pair; });
  return true;
}

const LayaTokenizer::Merge* LayaTokenizer::FindMerge(uint32_t left, uint32_t right) const {
  const uint64_t key = static_cast<uint64_t>(left) << 32 | right;
  auto it = std::lower_bound(merges_.begin(), merges_.end(), key,
                             [](const Merge& m, uint64_t k) { return m.pair < k; });
  return it != merges_.end() && it->pair == key ? &*it : nullptr;
}

void LayaTokenizer::EncodePiece(const std::u32string& piece, std::vector<int64_t>& out) const {
  std::vector<uint32_t> symbols;
  for (char32_t cp : piece) {
    auto it = std::lower_bound(chars_.begin(), chars_.end(), static_cast<uint32_t>(cp),
                               [](const auto& e, uint32_t c) { return e.first < c; });
    if (it != chars_.end() && it->first == cp) {
      symbols.push_back(it->second);
      continue;
    }
    std::string bytes;
    AppendUtf8(cp, bytes);
    bool allBytes = true;
    for (unsigned char b : bytes) allBytes &= byteTokens_[b] != kMissing;
    if (allBytes) {
      for (unsigned char b : bytes) symbols.push_back(byteTokens_[b]);
    } else if (symbols.empty() || symbols.back() != special_.unk) {
      symbols.push_back(special_.unk);  // fuse_unk: consecutive unknowns become one <unk>
    }
  }
  // Repeatedly apply the lowest-ranked merge, leftmost first.
  for (;;) {
    size_t best = symbols.size();
    const Merge* bestMerge = nullptr;
    for (size_t i = 0; i + 1 < symbols.size(); ++i) {
      const Merge* m = FindMerge(symbols[i], symbols[i + 1]);
      if (m && (!bestMerge || m->rank < bestMerge->rank)) {
        bestMerge = m;
        best = i;
      }
    }
    if (!bestMerge) break;
    symbols[best] = bestMerge->result;
    symbols.erase(symbols.begin() + best + 1);
  }
  out.insert(out.end(), symbols.begin(), symbols.end());
}

std::vector<int64_t> LayaTokenizer::Encode(const std::string& utf8) const {
  std::vector<int64_t> out;
  std::u32string text = DecodeUtf8(utf8);
  if (text.empty()) return out;
  std::replace(text.begin(), text.end(), U' ', kMetaspace);
  if (text.front() != kMetaspace) text.insert(text.begin(), kMetaspace);
  // Split before every ▁, which stays with the piece it starts.
  size_t start = 0;
  for (size_t i = 1; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == kMetaspace) {
      EncodePiece(text.substr(start, i - start), out);
      start = i;
    }
  }
  return out;
}

}  // namespace llaime
