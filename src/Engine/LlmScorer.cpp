#include "LlmScorer.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "ggml-backend.h"
#include "llama.h"

namespace llaime {
namespace {

// log-softmax value of `token` in `logits`.
double LogProb(const float* logits, int vocabSize, int token) {
  float maxLogit = logits[0];
  for (int i = 1; i < vocabSize; ++i) maxLogit = std::max(maxLogit, logits[i]);
  double sum = 0;
  for (int i = 0; i < vocabSize; ++i) sum += std::exp(static_cast<double>(logits[i] - maxLogit));
  return static_cast<double>(logits[token] - maxLogit) - std::log(sum);
}

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

bool LlmScorer::Load(const Options& options, std::string* error) {
  options_ = options;
  auto fail = [&](const char* message) {
    if (error) *error = message;
    return false;
  };

  llama_model_params mp = llama_model_default_params();
  std::vector<ggml_backend_dev_t> devices;
  const bool cpuOnly = options.device == "CPU";
  if (cpuOnly) {
    mp.n_gpu_layers = 0;
  } else {
    mp.n_gpu_layers = 999;
    if (options.device != "auto") {
      ggml_backend_dev_t dev = ggml_backend_dev_by_name(options.device.c_str());
      if (!dev) return fail("device not found");
      devices = {dev, nullptr};
      mp.devices = devices.data();
    }
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
                                     const std::vector<std::string>& candidates) {
  if (!context_ || candidates.empty()) return {};
  const size_t count = std::min(candidates.size(), static_cast<size_t>(options_.maxCandidates));

  // BOS (when the model uses one) starts the context; candidates continue it.
  std::vector<int> contextTokens = Tokenize(context, true);
  if (contextTokens.empty()) return {};
  if (!DecodeContext(contextTokens)) return {};

  llama_memory_t mem = llama_get_memory(context_);
  std::vector<std::vector<int>> candTokens(count);
  int total = 0;
  for (size_t k = 0; k < count; ++k) {
    candTokens[k] = Tokenize(candidates[k], false);
    total += static_cast<int>(candTokens[k].size());
    llama_memory_seq_rm(mem, static_cast<llama_seq_id>(k + 1), -1, -1);
    llama_memory_seq_cp(mem, 0, static_cast<llama_seq_id>(k + 1), -1, -1);
  }

  llama_batch batch = llama_batch_init(total, 0, 1);
  std::vector<int> firstIndex(count);
  const llama_pos base = static_cast<llama_pos>(contextTokens.size());
  for (size_t k = 0; k < count; ++k) {
    firstIndex[k] = batch.n_tokens;
    for (size_t t = 0; t < candTokens[k].size(); ++t) {
      const int j = batch.n_tokens++;
      batch.token[j] = candTokens[k][t];
      batch.pos[j] = base + static_cast<llama_pos>(t);
      batch.n_seq_id[j] = 1;
      batch.seq_id[j][0] = static_cast<llama_seq_id>(k + 1);
      // Logits at token t predict token t+1; the last token's prediction is not needed.
      batch.logits[j] = t + 1 < candTokens[k].size();
    }
  }
  std::vector<double> scores(count, 0.0);
  const bool ok = llama_decode(context_, batch) == 0;
  if (ok) {
    for (size_t k = 0; k < count; ++k) {
      if (candTokens[k].empty()) continue;
      double s = LogProb(contextLogits_.data(), vocabSize_, candTokens[k][0]);
      for (size_t t = 1; t < candTokens[k].size(); ++t) {
        const float* logits = llama_get_logits_ith(context_, firstIndex[k] + static_cast<int>(t) - 1);
        s += LogProb(logits, vocabSize_, candTokens[k][t]);
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
