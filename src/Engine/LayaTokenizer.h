#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace llaime {

// The mmBERT tokenizer Laya uses, from tokenizer.bin (tools/export_laya_tokenizer.py): spaces
// become ▁, a ▁ is prepended, the text is split before every ▁, and each piece is encoded with
// BPE over characters, unknown characters falling back to their UTF-8 byte tokens. Produces the
// same ids as Hugging Face tokenizers with add_special_tokens=False.
class LayaTokenizer {
 public:
  struct Special {
    uint32_t unk = 0, cls = 0, sep = 0, mask = 0, pad = 0;
  };

  bool Load(const std::wstring& path, std::string* error = nullptr);

  std::vector<int64_t> Encode(const std::string& utf8) const;

  const Special& special() const { return special_; }
  // Input budgets of the checkpoint (rl_agent_config.json) and its question text.
  uint32_t maxLen() const { return maxLen_; }
  uint32_t headMaxLen() const { return headMaxLen_; }
  const std::string& instruction() const { return instruction_; }

 private:
  struct Merge {
    uint64_t pair;  // left << 32 | right
    uint32_t rank;
    uint32_t result;
  };

  void EncodePiece(const std::u32string& piece, std::vector<int64_t>& out) const;
  const Merge* FindMerge(uint32_t left, uint32_t right) const;

  Special special_;
  uint32_t maxLen_ = 0, headMaxLen_ = 0;
  std::string instruction_;
  uint32_t byteTokens_[256] = {};
  std::vector<std::pair<uint32_t, uint32_t>> chars_;  // codepoint -> id, sorted
  std::vector<Merge> merges_;                          // sorted by pair
};

}  // namespace llaime
