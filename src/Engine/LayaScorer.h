#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "LayaTokenizer.h"

namespace Ort {
struct Env;
struct Session;
}  // namespace Ort

namespace llaime {

// Ranks candidate sentences with a fine-tuned Laya checkpoint exported to ONNX (stage 4b): one
// `choice` question whose options are the candidates, answered in a single forward pass on the
// CPU through ONNX Runtime. Much faster than a causal LM on machines without a GPU.
//
// Model directory: laya.int8.onnx (tools/prune_laya_vocab.py) and tokenizer.bin
// (tools/export_laya_tokenizer.py).
class LayaScorer {
 public:
  struct Options {
    std::wstring modelDir;
    std::wstring modelFile = L"laya.int8.onnx";
    int threads = 0;  // 0: ONNX Runtime default
  };

  LayaScorer();
  ~LayaScorer();
  LayaScorer(const LayaScorer&) = delete;
  LayaScorer& operator=(const LayaScorer&) = delete;

  bool Load(const Options& options, std::string* error = nullptr);
  bool IsLoaded() const { return session_ != nullptr; }

  // The state the model was trained with for this context (the text before the composition).
  static std::string State(const std::string& context);

  // Input ids and the [MASK] position of each option, built like Laya's build_sequence.
  void BuildInput(const std::string& state, const std::vector<std::string>& options,
                  std::vector<int64_t>* ids, std::vector<int64_t>* markers) const;

  // Natural-log probability of each candidate (UTF-8) being the intended sentence, given the
  // text before it. Empty on failure.
  std::vector<double> Score(const std::string& context, const std::vector<std::string>& candidates);

  const LayaTokenizer& tokenizer() const { return tokenizer_; }

 private:
  LayaTokenizer tokenizer_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
};

}  // namespace llaime
