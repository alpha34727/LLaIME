#pragma once
#include <memory>
#include <string>
#include <vector>

struct llama_model;
struct llama_context;
struct llama_vocab;

namespace llaime {

// A compute device llama.cpp can run on (from the dynamically loaded ggml backends).
struct LlmDevice {
  std::string name;         // e.g. "CPU", "CUDA0", "Vulkan0"
  std::string description;  // e.g. "NVIDIA GeForce RTX 5060 Ti"
  size_t memoryTotal = 0;
  bool isGpu = false;
};

// Scores candidate continuations of a text with a causal language model (llama.cpp):
// log P(candidate | context). The context is decoded once, then copied to one sequence per
// candidate so all candidates are evaluated in a single batch.
class LlmScorer {
 public:
  struct Options {
    std::string modelPath;      // GGUF file (UTF-8)
    std::string device = "auto";  // "auto", "CPU", or a device name such as "CUDA0"
    int threads = 0;            // 0: llama.cpp default
    int contextTokens = 512;    // context window per sequence
    int maxCandidates = 16;
    // How candidates are batched: "trie" (shared prefixes decoded once), "padded" (one sequence
    // each, padded to equal length), or "auto": padded for recurrent and hybrid models.
    std::string batchLayout = "auto";
  };

  LlmScorer();
  ~LlmScorer();
  LlmScorer(const LlmScorer&) = delete;
  LlmScorer& operator=(const LlmScorer&) = delete;

  // Loads ggml backends from `backendDir` (where ggml-*.dll live) once per process.
  static void LoadBackends(const std::string& backendDir);
  static std::vector<LlmDevice> Devices();
  // The device "auto" means: the first CUDA device, else the first discrete GPU (Vulkan on AMD
  // or Intel cards), else an integrated GPU, else "CPU". One device only: an NVIDIA card is
  // listed by both the CUDA and the Vulkan backend, and must not be split across the two.
  static std::string AutoDevice();

  bool Load(const Options& options, std::string* error = nullptr);
  bool IsLoaded() const { return context_ != nullptr; }
  // The device in use ("auto" resolved by Load).
  const std::string& device() const { return options_.device; }

  // Natural-log probability of each candidate (UTF-8) following `context` (UTF-8). With
  // `following` (the text after the candidate), the probability of the candidate followed by
  // that text: log P(candidate + following | context), tokenized together.
  // Returns an empty vector on failure.
  std::vector<double> Score(const std::string& context, const std::vector<std::string>& candidates,
                            const std::string& following = {});

  // Where the last Score() call spent its time, in milliseconds.
  struct Timing {
    double context = 0;     // tokenizing and decoding the context
    double candidates = 0;  // decoding the candidate trie
    double softmax = 0;     // log-sum-exp over the vocabulary
    int batchTokens = 0;
    int softmaxRows = 0;
  };
  const Timing& LastTiming() const { return timing_; }

 private:
  std::vector<int> Tokenize(const std::string& text, bool addSpecial) const;
  bool DecodeContext(const std::vector<int>& tokens);

  Options options_;
  llama_model* model_ = nullptr;
  llama_context* context_ = nullptr;
  const llama_vocab* vocab_ = nullptr;
  int vocabSize_ = 0;
  std::vector<int> cachedContext_;  // tokens currently held by sequence 0
  std::vector<float> contextLogits_;  // logits after the last context token
  bool paddedBatch_ = false;
  size_t softmaxThreads_ = 1;
  Timing timing_;
};

}  // namespace llaime
