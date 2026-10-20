// LLaIME.Engine.exe: per-user background process that ranks conversion candidates with a local
// language model for every app's TIP, over a named pipe.
//
// Settings: %LOCALAPPDATA%\LLaIME\engine.ini
//   [model]
//   ranker=none          ; the traditional algorithm: no model, the lexicon's order
//   ranker=auto          ; llm: causal LM (llama.cpp), best with a GPU
//                        ; laya: CPU mode, the Laya System 1 ranker (ONNX Runtime)
//                        ; auto: llm when a GPU is present, otherwise laya
//   path=C:\...\Qwen3.5-2B-Q4_K_M.gguf
//   device=auto          ; auto, CPU, CUDA0, Vulkan0, ...
//   threads=0            ; CPU threads, 0: llama.cpp default
//   [laya]
//   path=C:\...\models\laya-zh  ; laya.int8.onnx + tokenizer.bin
//   threads=0            ; 0: ONNX Runtime default
// Command line overrides: --ranker <name> --model <path> --device <name> --laya <dir>
//                         --userdb <path> (instead of user.db, for tests)
//                         --settings <path> (instead of engine.ini, for tests)
// The settings app writes engine.ini and sends ReloadRequest; the ranker is then unloaded and
// loaded again with the new settings (command line overrides still apply).
//
// The user dictionary (phrases and learned picks) lives in %LOCALAPPDATA%\LLaIME\user.db
// (UserStore). The pipe is served from the start; the model loads in the background, and until
// it is ready (or if none can load) rank requests are declined and the TIP keeps the lexicon's
// conversion.
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <memory>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "LayaScorer.h"
#include "LlmScorer.h"
#include "UserStore.h"
#include "llaime/Protocol.h"

using namespace llaime;
namespace proto = llaime::protocol;

namespace {

constexpr DWORD kPipeBuffer = 64 * 1024;
// Fixed preamble that steers the model toward Taiwan Traditional Chinese usage.
constexpr char kPromptPrefix[] = "以下是一段台灣繁體中文的文字：\n";
constexpr size_t kCacheEntries = 256;
// Laya log-probabilities are scaled so the TIP's fixed lexicon weight (0.75) gives the best
// measured mix for Laya, lexicon weight 1.0 (tests/eval/RESULTS.md).
constexpr double kLayaScale = 0.75;

std::wstring FromUtf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string ToUtf8(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                              nullptr, nullptr);
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}

std::wstring ExeDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring p(path);
  return p.substr(0, p.find_last_of(L"\\/"));
}

// %LOCALAPPDATA%\LLaIME
std::wstring DataDir() {
  PWSTR local = nullptr;
  std::wstring path;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
    path = std::wstring(local) + L"\\LLaIME";
    CoTaskMemFree(local);
  }
  return path;
}

// --settings replaces it (tests).
std::wstring& SettingsOverride() {
  static std::wstring path;
  return path;
}

std::wstring SettingsPath() {
  return SettingsOverride().empty() ? DataDir() + L"\\engine.ini" : SettingsOverride();
}

std::wstring ReadSetting(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) {
  wchar_t buf[1024];
  GetPrivateProfileStringW(section, key, fallback, buf, 1024, SettingsPath().c_str());
  return buf;
}

// Scores are pure functions of (context, candidates), so repeated requests (the same keystroke
// state re-sent, Backspace then retype) are answered from a small LRU cache. Requests from
// private fields are never cached.
class ScoreCache {
 public:
  bool Get(const std::wstring& key, std::vector<double>* scores) {
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    order_.splice(order_.begin(), order_, it->second.second);
    *scores = it->second.first;
    return true;
  }
  void Put(const std::wstring& key, std::vector<double> scores) {
    if (map_.count(key)) return;
    order_.push_front(key);
    map_[key] = {std::move(scores), order_.begin()};
    if (map_.size() > kCacheEntries) {
      map_.erase(order_.back());
      order_.pop_back();
    }
  }

 private:
  std::list<std::wstring> order_;
  std::unordered_map<std::wstring, std::pair<std::vector<double>, std::list<std::wstring>::iterator>>
      map_;
};

class Engine {
 public:
  struct Settings {
    std::wstring ranker = L"auto";
    std::wstring model, device = L"auto";
    int threads = 0;
    std::wstring layaDir;
    int layaThreads = 0;
    std::wstring layaDevice = L"CPU";  // or CUDA<n>
  };

  bool Start(const Settings& settings) {
    LlmScorer::LoadBackends(ToUtf8(ExeDir()));
    std::wstring ranker = settings.ranker;
    if (ranker == L"none") {
      // The traditional algorithm: the lexicon's own order, no model to load.
      rankerName_ = L"none";
      modelName_.clear();
      deviceName_.clear();
      return true;
    }
    if (ranker == L"auto") {
      bool gpu = false;
      for (const auto& d : LlmScorer::Devices()) gpu |= d.isGpu;
      ranker = gpu && !settings.model.empty() ? L"llm" : L"laya";
    }
    // Fall back to the other ranker when the chosen one cannot load.
    if (ranker == L"laya") return StartLaya(settings) || StartLlm(settings);
    return StartLlm(settings) || StartLaya(settings);
  }

  // A GPU backend that kills the process while loading (a CUDA build without kernels for this
  // card, a broken driver) would otherwise kill every start of the engine. Loading is marked
  // in a file; finding the mark at the next start means the load crashed, and the same device,
  // model and backend build are then skipped in favor of the CPU. Changing the device, the
  // model or the backend files tries again.
  static std::string FileText(const std::wstring& path) {
    std::ifstream in{std::filesystem::path(path), std::ios::binary};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  static void WriteText(const std::wstring& path, const std::string& text) {
    std::ofstream out{std::filesystem::path(path), std::ios::binary | std::ios::trunc};
    out << text;
  }

  static std::string LoadKey(const Settings& settings) {
    std::string key = ToUtf8(settings.device) + "|" + ToUtf8(settings.model);
    for (const wchar_t* dll : {L"ggml-cuda.dll", L"ggml-vulkan.dll"}) {
      std::error_code ec;
      const std::filesystem::path file = std::filesystem::path(ExeDir()) / dll;
      const auto size = std::filesystem::file_size(file, ec);
      const auto time = std::filesystem::last_write_time(file, ec);
      if (!ec) {
        key += "|" + std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
      }
    }
    return key;
  }

  bool StartLlm(const Settings& settings) {
    if (settings.model.empty()) return false;
    LlmScorer::Options options;
    options.modelPath = ToUtf8(settings.model);
    options.device = ToUtf8(settings.device);
    const std::wstring loadingFile = DataDir() + L"\\llm-loading.txt";
    const std::wstring failedFile = DataDir() + L"\\llm-failed.txt";
    const std::string key = LoadKey(settings);
    std::error_code ec;
    if (std::filesystem::exists(loadingFile, ec)) {
      WriteText(failedFile, FileText(loadingFile));  // the last load never finished
      std::filesystem::remove(loadingFile, ec);
    }
    Settings used = settings;
    if (settings.device != L"CPU" && FileText(failedFile) == key) {
      fprintf(stderr, "this device crashed the engine while loading; using the CPU\n");
      SetError(std::string("上次用「") + ToUtf8(settings.device) +
               "」載入模型時選字引擎當機，已改用 CPU。更換裝置、模型或更新版本後會再試。");
      options.device = "CPU";
      used.device = L"CPU";
    }
    WriteText(loadingFile, key);
    options.maxCandidates = 16;
    options.threads = settings.threads;
    std::string error;
    if (!scorer_->Load(options, &error)) {
      fprintf(stderr, "model load failed: %s\n", error.c_str());
      SetError("LLM: " + error);
      std::filesystem::remove(loadingFile, ec);
      return false;
    }
    modelName_ = settings.model.substr(settings.model.find_last_of(L"\\/") + 1);
    deviceName_ = FromUtf8(scorer_->device());  // "auto" resolved
    rankerName_ = L"llm";
    // The first decode allocates GPU buffers and compiles kernels; do it now rather than on
    // the user's first keystroke.
    scorer_->Score(kPromptPrefix, {"暖機", "暖身"});
    std::filesystem::remove(loadingFile, ec);
    return true;
  }

  bool StartLaya(const Settings& settings) {
    if (settings.layaDir.empty()) return false;
    LayaScorer::Options options;
    options.modelDir = settings.layaDir;
    options.threads = settings.layaThreads;
    options.device = ToUtf8(settings.layaDevice);
    std::string error;
    if (!laya_->Load(options, &error) && options.device != "CPU") {
      // No usable CUDA (driver, GPU): Laya on the CPU still works.
      SetError("Laya (" + options.device + "): " + error);
      options.device = "CPU";
      laya_ = std::make_unique<LayaScorer>();
      error.clear();
      if (laya_->Load(options, &error)) error.clear();
    }
    if (!laya_->IsLoaded()) {
      fprintf(stderr, "laya load failed: %s\n", error.c_str());
      SetError("Laya: " + error);
      return false;
    }
    modelName_ = L"Laya (" + settings.layaDir.substr(settings.layaDir.find_last_of(L"\\/") + 1) +
                 L")";
    deviceName_ = FromUtf8(options.device);
    rankerName_ = L"laya";
    laya_->Score("", {"暖機", "暖身"});
    return true;
  }

  // Loads the ranker on a background thread; requests are declined until it is ready. Called
  // again (ReloadRequest), it unloads the current ranker first.
  void StartInBackground(Settings settings) {
    std::thread([this, settings] {
      std::lock_guard<std::mutex> reload(reloadMutex_);
      state_ = proto::StatusResponse::Loading;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        scorer_ = std::make_unique<LlmScorer>();
        laya_ = std::make_unique<LayaScorer>();
        cache_ = ScoreCache();
      }
      {
        std::lock_guard<std::mutex> lock(errorMutex_);
        error_.clear();
      }
      if (Start(settings)) {
        state_ = proto::StatusResponse::Ready;
        fprintf(stderr, "LLaIME engine ready: %ls\n", modelName_.c_str());
      } else {
        state_ = proto::StatusResponse::Failed;
        fprintf(stderr, "no ranker could be loaded (%ls)\n", SettingsPath().c_str());
      }
    }).detach();
  }

  bool OpenUserStore(const std::wstring& path) {
    std::string error;
    if (store_.Open(path, &error)) return true;
    fprintf(stderr, "user.db: %s\n", error.c_str());
    return false;
  }

  bool IsLoaded() const { return state_ == proto::StatusResponse::Ready; }

  std::vector<uint8_t> Handle(const std::vector<uint8_t>& message) {
    proto::Reader in(message.data(), message.size());
    auto type = in.Header();
    switch (type.value_or(proto::MessageType::Ack)) {
      case proto::MessageType::StatusRequest: {
        proto::StatusResponse status;
        status.modelLoaded = IsLoaded();
        status.rankRequests = rankRequests_;
        status.privateRankRequests = privateRankRequests_;
        status.contextRankRequests = contextRankRequests_;
        status.learnRequests = learnRequests_;
        status.state = state_;
        if (status.modelLoaded) {
          status.model = modelName_;
          status.device = deviceName_;
          status.ranker = rankerName_;
        }
        {
          std::lock_guard<std::mutex> lock(errorMutex_);
          status.error = error_;
        }
        return proto::Encode(status);
      }
      case proto::MessageType::RankRequest:
        return HandleRank(in);
      case proto::MessageType::UserDictRequest: {
        proto::UserDictRequest request;
        if (!proto::Decode(in, &request)) break;
        return proto::Encode(store_.Snapshot(request.knownVersion));
      }
      case proto::MessageType::LearnRequest: {
        proto::LearnRequest request;
        proto::Ack ack{proto::MessageType::LearnRequest};
        if (proto::Decode(in, &request)) {
          ++learnRequests_;
          ack.ok = store_.Learn(request);
        }
        ack.userVersion = store_.Version();
        return proto::Encode(ack);
      }
      case proto::MessageType::PhraseRequest: {
        proto::PhraseRequest request;
        proto::Ack ack{proto::MessageType::PhraseRequest};
        if (proto::Decode(in, &request)) {
          ack.text = request.text;
          ack.ok = request.op == proto::PhraseRequest::Add
                       ? store_.AddPhrase(request.reading, request.text)
                       : store_.RemovePhrase(request.reading, request.text);
        }
        ack.userVersion = store_.Version();
        return proto::Encode(ack);
      }
      case proto::MessageType::SetLearningRequest: {
        proto::SetLearningRequest request;
        proto::Ack ack{proto::MessageType::SetLearningRequest};
        if (proto::Decode(in, &request)) ack.ok = store_.SetLearningEnabled(request.enabled);
        ack.userVersion = store_.Version();
        return proto::Encode(ack);
      }
      case proto::MessageType::DevicesRequest: {
        proto::DevicesResponse response;
        for (const auto& d : LlmScorer::Devices()) {
          response.devices.push_back({FromUtf8(d.name), FromUtf8(d.description), d.memoryTotal,
                                      d.isGpu});
        }
        return proto::Encode(response);
      }
      case proto::MessageType::ReloadRequest: {
        StartInBackground(reloadSettings_());
        proto::Ack ack{proto::MessageType::ReloadRequest};
        ack.ok = true;
        ack.userVersion = store_.Version();
        return proto::Encode(ack);
      }
      default:
        break;
    }
    return proto::Encode(proto::Ack{});
  }

 private:
  std::vector<uint8_t> HandleRank(proto::Reader& in) {
    proto::RankResponse response;
    proto::RankRequest request;
    response.userVersion = store_.Version();
    if (!proto::Decode(in, &request)) return proto::Encode(response);
    response.requestId = request.requestId;
    ++rankRequests_;
    if (request.privateMode) ++privateRankRequests_;
    if (!request.context.empty()) ++contextRankRequests_;
    std::wstring key = request.context;
    for (const auto& c : request.candidates) key += L'\x1f' + c;
    std::lock_guard<std::mutex> lock(mutex_);
    // Checked under the lock: a reload replaces the rankers while holding it.
    if (!IsLoaded() || rankerName_ == L"none") return proto::Encode(response);
    if (!request.privateMode && cache_.Get(key, &response.scores)) {
      response.ok = true;
      return proto::Encode(response);
    }
    std::vector<std::string> candidates;
    for (const auto& c : request.candidates) candidates.push_back(ToUtf8(c));
    if (laya_->IsLoaded()) {
      response.scores = laya_->Score(ToUtf8(request.context), candidates);
      for (double& score : response.scores) score *= kLayaScale;
    } else {
      response.scores = scorer_->Score(kPromptPrefix + ToUtf8(request.context), candidates);
    }
    response.ok = response.scores.size() == request.candidates.size();
    if (response.ok && !request.privateMode) cache_.Put(key, response.scores);
    return proto::Encode(response);
  }

  void SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    if (!error_.empty()) error_ += L"\n";
    error_ += FromUtf8(error);
  }

 public:
  // Reads the settings again for a reload (engine.ini, then the command line).
  std::function<Settings()> reloadSettings_;

 private:
  std::unique_ptr<LlmScorer> scorer_ = std::make_unique<LlmScorer>();
  std::unique_ptr<LayaScorer> laya_ = std::make_unique<LayaScorer>();
  UserStore store_;
  std::atomic<proto::StatusResponse::State> state_ = proto::StatusResponse::Loading;
  std::mutex reloadMutex_;  // one load at a time
  std::mutex errorMutex_;
  std::wstring error_, rankerName_;
  std::atomic<uint32_t> rankRequests_ = 0, privateRankRequests_ = 0, contextRankRequests_ = 0,
                        learnRequests_ = 0;
  std::mutex mutex_;  // one llama context: requests are scored one at a time
  ScoreCache cache_;
  std::wstring modelName_, deviceName_;
};

// Lets the user's own processes connect, including AppContainer apps (ALL APPLICATION
// PACKAGES) and low-integrity processes.
bool MakePipeSecurity(const std::wstring& sid, SECURITY_ATTRIBUTES* sa, PSECURITY_DESCRIPTOR* sd) {
  const std::wstring sddl =
      L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")(A;;GRGW;;;AC)S:(ML;;NW;;;LW)";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, sd,
                                                            nullptr)) {
    return false;
  }
  sa->nLength = sizeof(*sa);
  sa->lpSecurityDescriptor = *sd;
  sa->bInheritHandle = FALSE;
  return true;
}

void ServeClient(Engine* engine, HANDLE pipe) {
  std::vector<uint8_t> buffer(kPipeBuffer);
  for (;;) {
    std::vector<uint8_t> message;
    DWORD read = 0;
    BOOL ok;
    do {
      ok = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
      if (!ok && GetLastError() != ERROR_MORE_DATA) {
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        return;
      }
      message.insert(message.end(), buffer.begin(), buffer.begin() + read);
    } while (!ok);
    const auto reply = engine->Handle(message);
    DWORD written = 0;
    if (!WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &written, nullptr)) {
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      return;
    }
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  auto readSettings = [argc, argv] {
    Engine::Settings settings;
    settings.ranker = ReadSetting(L"model", L"ranker", L"auto");
    settings.model = ReadSetting(L"model", L"path", L"");
    settings.device = ReadSetting(L"model", L"device", L"auto");
    settings.threads = _wtoi(ReadSetting(L"model", L"threads", L"0").c_str());
    settings.layaDir = ReadSetting(L"laya", L"path", L"");
    settings.layaThreads = _wtoi(ReadSetting(L"laya", L"threads", L"0").c_str());
    settings.layaDevice = ReadSetting(L"laya", L"device", L"CPU");
    for (int i = 1; i + 1 < argc; i += 2) {
      std::wstring key = argv[i];
      if (key == L"--ranker") settings.ranker = argv[i + 1];
      if (key == L"--model") settings.model = argv[i + 1];
      if (key == L"--device") settings.device = argv[i + 1];
      if (key == L"--laya") settings.layaDir = argv[i + 1];
    }
    return settings;
  };
  std::wstring userDb = DataDir() + L"\\user.db";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::wstring(argv[i]) == L"--userdb") userDb = argv[i + 1];
    if (std::wstring(argv[i]) == L"--settings") SettingsOverride() = argv[i + 1];
  }

  const std::wstring sid = proto::CurrentUserSid();
  // One engine per user session; the mutex is held for the life of the process.
  CreateMutexW(nullptr, TRUE, (L"Local\\LLaIME.Engine." + sid).c_str());
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

  Engine engine;
  std::error_code ec;
  std::filesystem::create_directories(DataDir(), ec);
  engine.OpenUserStore(userDb);
  engine.reloadSettings_ = readSettings;
  engine.StartInBackground(readSettings());

  SECURITY_ATTRIBUTES sa{};
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!MakePipeSecurity(sid, &sa, &sd)) return 1;
  const std::wstring pipeName = proto::PipeName(sid);
  for (;;) {
    HANDLE pipe = CreateNamedPipeW(
        pipeName.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, kPipeBuffer, kPipeBuffer, 0, &sa);
    if (pipe == INVALID_HANDLE_VALUE) {
      Sleep(1000);
      continue;
    }
    if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
      std::thread(ServeClient, &engine, pipe).detach();
    } else {
      CloseHandle(pipe);
    }
  }
  // Serves until the process is ended (logoff, or the installer stopping it).
}
