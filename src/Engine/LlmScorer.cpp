#include "LlmScorer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#include "ggml-backend.h"
#include "llama.h"

namespace llaime {
namespace {

// log(sum(exp(logits))) over the vocabulary; written so the compiler can vectorize it.
float LogSumExp(const float* logits, int vocabSize) {
  float maxLogit = logits[0];
  for (int i = 1; i < vocabSize; ++i) maxLogit = logits[i] > maxLogit ? logits[i] : maxLogit;
  float sum = 0;
  for (int i = 0; i < vocabSize; ++i) sum += std::exp(logits[i] - maxLogit);
  return maxLogit + std::log(sum);
}

// Candidates as a token trie: shared prefixes are decoded once, with every candidate passing
// through a node listed as one of its sequences.
struct TrieNode {
  int token;
  int parent;  // -1: child of the context
  int depth;
  std::vector<llama_seq_id> seqs;
  bool hasChildren = false;
  int batchIndex = -1;
};

void Silence(ggml_log_level level, const char* text, void*) {
  // Keep llama.cpp quiet except for errors.
  if (level == GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
}

}  // namespace

LlmScorer::LlmScorer() = default;

LlmScorer::~LlmScorer() {
  if (context_) llama_free(context_);
  if (model_) llama_model_free(model_);
}

void LlmScorer::LoadBackends(const std::string& backendDir) {
  static std::once_flag once;
  std::call_once(once, [&] {
    llama_log_set(Silence, nullptr);
    llama_backend_init();
    if (backendDir.empty()) {
      ggml_backend_load_all();
    } else {
      ggml_backend_load_all_from_path(backendDir.c_str());
    }
  });
}

std::vector<LlmDevice> LlmScorer::Devices() {
  std::vector<LlmDevice> out;
  for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
    ggml_backend_dev_t dev = ggml_backend_dev_get(i);
    LlmDevice d;
    d.name = ggml_backend_dev_name(dev);
    d.description = ggml_backend_dev_description(dev);
    size_t freeMem = 0;
    ggml_backend_dev_memory(dev, &freeMem, &d.memoryTotal);
    const auto type = ggml_backend_dev_type(dev);
    d.isGpu = type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU;
    out.push_back(std::move(d));
  }
  return out;
}

std::string LlmScorer::AutoDevice() {
  std::string discrete, integrated;
  for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
    ggml_backend_dev_t dev = ggml_backend_dev_get(i);
    const std::string name = ggml_backend_dev_name(dev);
    const auto type = ggml_backend_dev_type(dev);
    if (type == GGML_BACKEND_DEVICE_TYPE_GPU && name.rfind("CUDA", 0) == 0) return name;
    if (type == GGML_BACKEND_DEVICE_TYPE_GPU && discrete.empty()) discrete = name;
    if (type == GGML_BACKEND_DEVICE_TYPE_IGPU && integrated.empty()) integrated = name;
  }
  return !discrete.empty() ? discrete : !integrated.empty() ? integrated : "CPU";
}

bool LlmScorer::Load(const Options& options, std::string* error) {
  options_ = options;
  auto fail = [&](const char* message) {
    if (error) *error = message;
    return false;
  };

  llama_model_params mp = llama_model_default_params();
  std::vector<ggml_backend_dev_t> devices;
  const std::string device = options.device == "auto" ? AutoDevice() : options.device;
  options_.device = device;
  const bool cpuOnly = device == "CPU";
  if (cpuOnly) {
    mp.n_gpu_layers = 0;
  } else {
    mp.n_gpu_layers = 999;
    ggml_backend_dev_t dev = ggml_backend_dev_by_name(device.c_str());
    if (!dev) return fail("device not found");
    devices = {dev, nullptr};
    mp.devices = devices.data();
  }
  model_ = llama_model_load_from_file(options.modelPath.c_str(), mp);
  if (!model_) return fail("failed to load model");
  vocab_ = llama_model_get_vocab(model_);
  vocabSize_ = llama_vocab_n_tokens(vocab_);

  llama_context_params cp = llama_context_default_params();
  // Sequence 0 holds the context; sequences 1..N hold one candidate each.
  cp.n_seq_max = static_cast<uint32_t>(options.maxCandidates + 1);
  cp.n_ctx = static_cast<uint32_t>(options.contextTokens * (options.maxCandidates + 1));
  cp.n_batch = cp.n_ctx;
  cp.n_ubatch = 512;
  cp.kv_unified = true;  // candidates share the context prefix
  cp.no_perf = true;
  if (options.threads > 0) {
    cp.n_threads = options.threads;
    cp.n_threads_batch = options.threads;
  }
  if (cpuOnly) cp.op_offload = false;
  context_ = llama_init_from_model(model_, cp);
  if (!context_) return fail("failed to create context");
  paddedBatch_ = options.batchLayout == "padded" ||
                 (options.batchLayout == "auto" &&
                  (llama_model_is_recurrent(model_) || llama_model_is_hybrid(model_)));
  softmaxThreads_ = std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 1, 8);
  return true;
}

std::vector<int> LlmScorer::Tokenize(const std::string& text, bool addSpecial) const {
  std::vector<llama_token> tokens(text.size() + 8);
  int n = llama_tokenize(vocab_, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
                         static_cast<int32_t>(tokens.size()), addSpecial, false);
  if (n < 0) {
    tokens.resize(-n);
    n = llama_tokenize(vocab_, text.data(), static_cast<int32_t>(text.size()), tokens.data(),
                       static_cast<int32_t>(tokens.size()), addSpecial, false);
  }
  tokens.resize(std::max(n, 0));
  return std::vector<int>(tokens.begin(), tokens.end());
}

bool LlmScorer::DecodeContext(const std::vector<int>& tokens) {
  llama_memory_t mem = llama_get_memory(context_);
  // Reuse the longest common prefix with what sequence 0 already holds. Recurrent/hybrid
  // models cannot drop a partial sequence; then the context is decoded from scratch.
  size_t common = 0;
  while (common < tokens.size() && common < cachedContext_.size() &&
         tokens[common] == cachedContext_[common]) {
    ++common;
  }
  if (common == tokens.size() && common == cachedContext_.size() && !contextLogits_.empty()) {
    return true;
  }
  // Re-decode at least the last token to get fresh logits for the first candidate token.
  common = std::min(common, tokens.empty() ? 0 : tokens.size() - 1);
  if (!llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(common), -1)) {
    llama_memory_seq_rm(mem, 0, -1, -1);
    common = 0;
  }
  cachedContext_.assign(tokens.begin(), tokens.begin() + common);

  llama_batch batch = llama_batch_init(static_cast<int32_t>(tokens.size() - common), 0, 1);
  for (size_t i = common; i < tokens.size(); ++i) {
    const int j = batch.n_tokens++;
    batch.token[j] = tokens[i];
    batch.pos[j] = static_cast<llama_pos>(i);
    batch.n_seq_id[j] = 1;
    batch.seq_id[j][0] = 0;
    batch.logits[j] = i + 1 == tokens.size();
  }
  const bool ok = batch.n_tokens == 0 || llama_decode(context_, batch) == 0;
  if (ok && batch.n_tokens > 0) {
    const float* logits = llama_get_logits_ith(context_, batch.n_tokens - 1);
    contextLogits_.assign(logits, logits + vocabSize_);
    cachedContext_ = tokens;
  } else {
    cachedContext_.clear();
    contextLogits_.clear();
  }
  llama_batch_free(batch);
  return ok;
}

std::vector<double> LlmScorer::Score(const std::string& context,
                                     const std::vector<std::string>& candidates,
                                     const std::string& following) {
  if (!context_ || candidates.empty()) return {};
  const size_t count = std::min(candidates.size(), static_cast<size_t>(options_.maxCandidates));

  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  timing_ = {};
  const auto t0 = Clock::now();
  // BOS (when the model uses one) starts the context; candidates continue it.
  std::vector<int> contextTokens = Tokenize(context, true);
  if (contextTokens.empty()) return {};
  if (!DecodeContext(contextTokens)) return {};
  const auto t1 = Clock::now();
  timing_.context = ms(t0, t1);

  llama_memory_t mem = llama_get_memory(context_);
  std::vector<std::vector<int>> candTokens(count);
  for (size_t k = 0; k < count; ++k) {
    candTokens[k] = Tokenize(candidates[k] + following, false);
    llama_memory_seq_rm(mem, static_cast<llama_seq_id>(k + 1), -1, -1);
    llama_memory_seq_cp(mem, 0, static_cast<llama_seq_id>(k + 1), -1, -1);
  }

  // Build the trie; path[k] lists the nodes of candidate k.
  std::vector<TrieNode> nodes;
  std::vector<std::vector<int>> path(count);
  for (size_t k = 0; k < count; ++k) {
    int parent = -1;
    for (size_t t = 0; t < candTokens[k].size(); ++t) {
      int found = -1;
      for (size_t n = 0; n < nodes.size(); ++n) {
        if (nodes[n].parent == parent && nodes[n].token == candTokens[k][t]) {
          found = static_cast<int>(n);
          break;
        }
      }
      if (found < 0) {
        nodes.push_back({candTokens[k][t], parent, static_cast<int>(t), {}});
        found = static_cast<int>(nodes.size()) - 1;
        if (parent >= 0) nodes[parent].hasChildren = true;
      }
      nodes[found].seqs.push_back(static_cast<llama_seq_id>(k + 1));
      path[k].push_back(found);
      parent = found;
    }
  }

  const llama_pos base = static_cast<llama_pos>(contextTokens.size());
  llama_batch batch;
  if (!paddedBatch_) {
    // Shared prefixes as one token in several sequences.
    batch = llama_batch_init(static_cast<int32_t>(nodes.size()), 0, static_cast<int32_t>(count));
    for (TrieNode& node : nodes) {
      const int j = batch.n_tokens++;
      node.batchIndex = j;
      batch.token[j] = node.token;
      batch.pos[j] = base + node.depth;
      batch.n_seq_id[j] = static_cast<int32_t>(node.seqs.size());
      for (size_t q = 0; q < node.seqs.size(); ++q) batch.seq_id[j][q] = node.seqs[q];
      // A node's logits predict its children.
      batch.logits[j] = node.hasChildren;
    }
  } else {
    // Recurrent layers (Qwen3.5's linear attention) split a batch into ubatches of equal-length,
    // non-overlapping sequence sets, so shared trie tokens would cost one model pass per depth
    // and branch. Instead every candidate is its own sequence, padded to the longest with its
    // last token (positions past its end are never read): one ubatch for all. Logits are still
    // taken once per trie node, from the first candidate through it.
    size_t longest = 0;
    for (const auto& tokens : candTokens) longest = std::max(longest, tokens.size());
    batch = llama_batch_init(static_cast<int32_t>(count * longest), 0, 1);
    for (size_t k = 0; k < count; ++k) {
      for (size_t t = 0; t < longest; ++t) {
        const int j = batch.n_tokens++;
        const bool real = t < candTokens[k].size();
        batch.token[j] = real ? candTokens[k][t] : candTokens[k].back();
        batch.pos[j] = base + static_cast<llama_pos>(t);
        batch.n_seq_id[j] = 1;
        batch.seq_id[j][0] = static_cast<llama_seq_id>(k + 1);
        batch.logits[j] = false;
        if (!real) continue;
        TrieNode& node = nodes[path[k][t]];
        if (node.hasChildren && node.seqs[0] == static_cast<llama_seq_id>(k + 1)) {
          node.batchIndex = j;
          batch.logits[j] = true;
        }
      }
    }
  }
  std::vector<double> scores(count, 0.0);
  const bool ok = llama_decode(context_, batch) == 0;
  // Logits are read back lazily; force completion before timing the softmax.
  if (ok) llama_synchronize(context_);
  const auto t2 = Clock::now();
  timing_.candidates = ms(t1, t2);
  timing_.batchTokens = batch.n_tokens;
  if (ok) {
    // One log-sum-exp per distinct prediction row: the context row plus internal nodes.
    const float contextLse = LogSumExp(contextLogits_.data(), vocabSize_);
    std::vector<float> nodeLse(nodes.size(), 0);
    // (node, its logits); llama_get_logits_ith may reorder the outputs, so only this thread
    // calls it.
    std::vector<std::pair<size_t, const float*>> rows;
    for (size_t n = 0; n < nodes.size(); ++n) {
      if (nodes[n].hasChildren) rows.emplace_back(n, llama_get_logits_ith(context_, nodes[n].batchIndex));
    }
    // Each row is a pass over the whole vocabulary (248k for Qwen3.5): spread them over threads.
    std::atomic<size_t> next{0};
    auto work = [&] {
      for (size_t r; (r = next.fetch_add(1)) < rows.size();) {
        nodeLse[rows[r].first] = LogSumExp(rows[r].second, vocabSize_);
      }
    };
    std::vector<std::thread> threads;
    const size_t helpers = std::min<size_t>(softmaxThreads_ - 1, rows.size() / 8);
    for (size_t i = 0; i < helpers; ++i) threads.emplace_back(work);
    work();
    for (auto& thread : threads) thread.join();
    timing_.softmax = ms(t2, Clock::now());
    for (const TrieNode& node : nodes) timing_.softmaxRows += node.hasChildren;
    for (size_t k = 0; k < count; ++k) {
      double s = 0;
      for (size_t t = 0; t < path[k].size(); ++t) {
        const TrieNode& node = nodes[path[k][t]];
        if (node.parent < 0) {
          s += contextLogits_[node.token] - contextLse;
        } else {
          const TrieNode& parent = nodes[node.parent];
          s += llama_get_logits_ith(context_, parent.batchIndex)[node.token] - nodeLse[node.parent];
        }
      }
      scores[k] = s;
    }
  }
  llama_batch_free(batch);
  for (size_t k = 0; k < count; ++k) {
    llama_memory_seq_rm(mem, static_cast<llama_seq_id>(k + 1), -1, -1);
  }
  if (!ok) return {};
  return scores;
}

}  // namespace llaime
