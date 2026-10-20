#include "LayaScorer.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numeric>

namespace llaime {
namespace {

// Laya's per-option token cap (build_head: truncation max_length=48).
constexpr size_t kOptionTokens = 48;

}  // namespace

LayaScorer::LayaScorer() = default;
LayaScorer::~LayaScorer() = default;

bool LayaScorer::Load(const Options& options, std::string* error) {
  std::string tokError;
  if (!tokenizer_.Load(options.modelDir + L"\\tokenizer.bin", &tokError)) {
    if (error) *error = "tokenizer: " + tokError;
    return false;
  }
  try {
    env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "LLaIME.Laya");
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (options.threads > 0) so.SetIntraOpNumThreads(options.threads);
    if (options.device.rfind("CUDA", 0) == 0) {
      // Nodes the CUDA provider has no kernel for (some of the INT8 ones) stay on the CPU.
      OrtCUDAProviderOptions cuda{};
      cuda.device_id = std::atoi(options.device.c_str() + 4);
      so.AppendExecutionProvider_CUDA(cuda);
    }
    // A GPU runs the fp16 graph (the INT8 one falls back to the CPU for most nodes); the CPU
    // runs INT8. Either graph is used when it is the only one in the folder.
    std::wstring file = options.modelFile;
    const bool gpu = options.device.rfind("CUDA", 0) == 0;
    auto exists = [&](const wchar_t* name) {
      std::error_code ec;
      return std::filesystem::exists(std::filesystem::path(options.modelDir) / name, ec);
    };
    if (file.empty()) {
      file = gpu ? (exists(L"laya.fp16.onnx") ? L"laya.fp16.onnx" : L"laya.int8.onnx")
                 : (exists(L"laya.int8.onnx") ? L"laya.int8.onnx" : L"laya.fp16.onnx");
    }
    modelFile_ = file;
    const std::wstring model = options.modelDir + L"\\" + file;
    session_ = std::make_unique<Ort::Session>(*env_, model.c_str(), so);
  } catch (const Ort::Exception& e) {
    if (error) *error = e.what();
    session_.reset();
    return false;
  }
  return true;
}

std::string LayaScorer::State(const std::string& context) {
  // tools/build_ranker_data.py state(): Laya skips cases with an empty state.
  return "\xE4\xB8\x8A\xE6\x96\x87\xEF\xBC\x9A" + (context.empty() ? "\xE7\x84\xA1" : context);
}

void LayaScorer::BuildInput(const std::string& state, const std::vector<std::string>& options,
                            std::vector<int64_t>* ids, std::vector<int64_t>* markers) const {
  const auto& sp = tokenizer_.special();
  const size_t headMax = tokenizer_.headMaxLen(), maxLen = tokenizer_.maxLen();
  // build_head: "[CLS] <type> question: instructions [SEP] [MASK] opt0 [MASK] opt1 ... [SEP]".
  std::vector<int64_t> head = tokenizer_.Encode("choice question: " + tokenizer_.instruction());
  std::vector<std::vector<int64_t>> optIds;
  size_t total = 0;
  for (const std::string& option : options) {
    std::vector<int64_t> o = tokenizer_.Encode(" " + option);
    if (o.size() > kOptionTokens) o.resize(kOptionTokens);
    o.insert(o.begin(), sp.mask);
    total += o.size();
    optIds.push_back(std::move(o));
  }
  long long budget = static_cast<long long>(headMax) - static_cast<long long>(total);
  if (budget < 16) {
    const size_t per = std::max<size_t>(4, (headMax - 16) / std::max<size_t>(1, optIds.size()));
    total = 0;
    for (auto& o : optIds) {
      if (o.size() > per) o.resize(per);
      total += o.size();
    }
    budget = static_cast<long long>(headMax) - static_cast<long long>(total);
  }
  head.resize(std::min<size_t>(head.size(), static_cast<size_t>(std::max<long long>(8, budget))));

  ids->assign(1, sp.cls);
  ids->insert(ids->end(), head.begin(), head.end());
  ids->push_back(sp.sep);
  markers->clear();
  for (const auto& o : optIds) {
    markers->push_back(static_cast<int64_t>(ids->size()));
    ids->insert(ids->end(), o.begin(), o.end());
  }
  ids->push_back(sp.sep);
  // build_sequence: the state fills what is left, then [SEP].
  const size_t room = maxLen > ids->size() + 1 ? maxLen - ids->size() - 1 : 0;
  std::vector<int64_t> st = tokenizer_.Encode(state);
  if (st.size() > room) st.resize(room);
  ids->insert(ids->end(), st.begin(), st.end());
  ids->push_back(sp.sep);
  if (ids->size() > maxLen) ids->resize(maxLen);
  std::erase_if(*markers, [&](int64_t m) { return m >= static_cast<int64_t>(maxLen); });
}

std::vector<double> LayaScorer::Score(const std::string& context,
                                      const std::vector<std::string>& candidates) {
  if (!session_ || candidates.size() < 2) return {};
  std::vector<int64_t> ids, markers;
  BuildInput(State(context), candidates, &ids, &markers);
  if (markers.size() != candidates.size()) return {};

  const int64_t length = static_cast<int64_t>(ids.size());
  const int64_t count = static_cast<int64_t>(markers.size());
  std::vector<int64_t> attention(ids.size(), 1);
  std::unique_ptr<bool[]> markerMask(new bool[markers.size()]);
  std::fill_n(markerMask.get(), markers.size(), true);
  int64_t qtype = 0;  // choice

  try {
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const int64_t seqShape[2] = {1, length}, markerShape[2] = {1, count}, typeShape[1] = {1};
    Ort::Value inputs[5] = {
        Ort::Value::CreateTensor<int64_t>(memory, ids.data(), ids.size(), seqShape, 2),
        Ort::Value::CreateTensor<int64_t>(memory, attention.data(), attention.size(), seqShape, 2),
        Ort::Value::CreateTensor<int64_t>(memory, markers.data(), markers.size(), markerShape, 2),
        Ort::Value::CreateTensor<bool>(memory, markerMask.get(), markers.size(), markerShape, 2),
        Ort::Value::CreateTensor<int64_t>(memory, &qtype, 1, typeShape, 1),
    };
    const char* inputNames[5] = {"input_ids", "attention_mask", "marker_pos", "marker_mask",
                                 "qtype"};
    const char* outputNames[1] = {"logits"};
    auto outputs = session_->Run(Ort::RunOptions{nullptr}, inputNames, inputs, 5, outputNames, 1);
    const float* logits = outputs[0].GetTensorData<float>();
    // log-softmax over the options
    const float top = *std::max_element(logits, logits + count);
    double sum = 0;
    for (int64_t i = 0; i < count; ++i) sum += std::exp(static_cast<double>(logits[i] - top));
    std::vector<double> scores(count);
    for (int64_t i = 0; i < count; ++i) scores[i] = logits[i] - top - std::log(sum);
    return scores;
  } catch (const Ort::Exception&) {
    return {};
  }
}

}  // namespace llaime
