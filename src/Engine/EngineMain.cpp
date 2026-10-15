// LLaIME.Engine.exe: per-user background process that ranks conversion candidates with a local
// language model for every app's TIP, over a named pipe.
//
// Settings: %LOCALAPPDATA%\LLaIME\engine.ini
//   [model]
//   ranker=auto          ; llm: causal LM (llama.cpp), best with a GPU
//                        ; laya: CPU mode, the Laya System 1 ranker (ONNX Runtime)
//                        ; auto: llm when a GPU is present, otherwise laya
//   path=C:\...\Qwen3.5-2B-Q4_K_M.gguf
//   device=auto          ; auto, CPU, CUDA0, Vulkan0, ...
//   [laya]
//   path=C:\...\models\laya-zh  ; laya.int8.onnx + tokenizer.bin
//   threads=0            ; 0: ONNX Runtime default
// Command line overrides: --ranker <name> --model <path> --device <name> --laya <dir>
//                         --userdb <path> (instead of user.db, for tests)
//
// The user dictionary (phrases and learned picks) lives in %LOCALAPPDATA%\LLaIME\user.db
// (UserStore). The pipe is served from the start; the model loads in the background, and until
// it is ready (or if none can load) rank requests are declined and the TIP keeps the lexicon's
// conversion.
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include <atomic>
#include <filesystem>
#include <cstdio>
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

std::wstring SettingsPath() { return DataDir() + L"\\engine.ini"; }

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
    std::wstring layaDir;
    int layaThreads = 0;
  };

  bool Start(const Settings& settings) {
    LlmScorer::LoadBackends(ToUtf8(ExeDir()));
    std::wstring ranker = settings.ranker;
    if (ranker == L"auto") {
      bool gpu = false;
      for (const auto& d : LlmScorer::Devices()) gpu |= d.isGpu;
      ranker = gpu && !settings.model.empty() ? L"llm" : L"laya";
    }
    // Fall back to the other ranker when the chosen one cannot load.
    if (ranker == L"laya") return StartLaya(settings) || StartLlm(settings);
    return StartLlm(settings) || StartLaya(settings);
  }

  bool StartLlm(const Settings& settings) {
    if (settings.model.empty()) return false;
    LlmScorer::Options options;
    options.modelPath = ToUtf8(settings.model);
    options.device = ToUtf8(settings.device);
    options.maxCandidates = 16;
    std::string error;
    if (!scorer_.Load(options, &error)) {
      fprintf(stderr, "model load failed: %s\n", error.c_str());
      return false;
    }
    modelName_ = settings.model.substr(settings.model.find_last_of(L"\\/") + 1);
    deviceName_ = settings.device;
    // The first decode allocates GPU buffers and compiles kernels; do it now rather than on
    // the user's first keystroke.
    scorer_.Score(kPromptPrefix, {"暖機", "暖身"});
    return true;
  }

  bool StartLaya(const Settings& settings) {
    if (settings.layaDir.empty()) return false;
    LayaScorer::Options options;
    options.modelDir = settings.layaDir;
    options.threads = settings.layaThreads;
    std::string error;
    if (!laya_.Load(options, &error)) {
      fprintf(stderr, "laya load failed: %s\n", error.c_str());
      return false;
    }
    modelName_ = L"Laya (" + settings.layaDir.substr(settings.layaDir.find_last_of(L"\\/") + 1) +
                 L")";
    deviceName_ = L"CPU";
    laya_.Score("", {"暖機", "暖身"});
    return true;
  }

  // Loads the ranker on a background thread; requests are declined until it is ready.
  void StartInBackground(Settings settings) {
    std::thread([this, settings] {
      if (Start(settings)) {
        ready_ = true;
        fprintf(stderr, "LLaIME engine ready: %ls\n", modelName_.c_str());
      } else {
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

  bool IsLoaded() const { return ready_ && (scorer_.IsLoaded() || laya_.IsLoaded()); }

  std::vector<uint8_t> Handle(const std::vector<uint8_t>& message) {
    proto::Reader in(message.data(), message.size());
    auto type = in.Header();
    switch (type.value_or(proto::MessageType::Ack)) {
      case proto::MessageType::StatusRequest: {
        proto::StatusResponse status;
        status.modelLoaded = IsLoaded();
        if (status.modelLoaded) {
          status.model = modelName_;
          status.device = deviceName_;
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
        if (proto::Decode(in, &request)) ack.ok = store_.Learn(request);
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
    if (!IsLoaded()) return proto::Encode(response);

    std::wstring key = request.context;
    for (const auto& c : request.candidates) key += L'\x1f' + c;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!request.privateMode && cache_.Get(key, &response.scores)) {
      response.ok = true;
      return proto::Encode(response);
    }
    std::vector<std::string> candidates;
    for (const auto& c : request.candidates) candidates.push_back(ToUtf8(c));
    if (laya_.IsLoaded()) {
      response.scores = laya_.Score(ToUtf8(request.context), candidates);
      for (double& score : response.scores) score *= kLayaScale;
    } else {
      response.scores = scorer_.Score(kPromptPrefix + ToUtf8(request.context), candidates);
    }
    response.ok = response.scores.size() == request.candidates.size();
    if (response.ok && !request.privateMode) cache_.Put(key, response.scores);
    return proto::Encode(response);
  }

  LlmScorer scorer_;
  LayaScorer laya_;
  UserStore store_;
  std::atomic<bool> ready_ = false;
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
  Engine::Settings settings;
  settings.ranker = ReadSetting(L"model", L"ranker", L"auto");
  settings.model = ReadSetting(L"model", L"path", L"");
  settings.device = ReadSetting(L"model", L"device", L"auto");
  settings.layaDir = ReadSetting(L"laya", L"path", L"");
  settings.layaThreads = _wtoi(ReadSetting(L"laya", L"threads", L"0").c_str());
  std::wstring userDb = DataDir() + L"\\user.db";
  for (int i = 1; i + 1 < argc; i += 2) {
    std::wstring key = argv[i];
    if (key == L"--ranker") settings.ranker = argv[i + 1];
    if (key == L"--model") settings.model = argv[i + 1];
    if (key == L"--device") settings.device = argv[i + 1];
    if (key == L"--laya") settings.layaDir = argv[i + 1];
    if (key == L"--userdb") userDb = argv[i + 1];
  }

  const std::wstring sid = proto::CurrentUserSid();
  // One engine per user session; the mutex is held for the life of the process.
  CreateMutexW(nullptr, TRUE, (L"Local\\LLaIME.Engine." + sid).c_str());
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

  Engine engine;
  std::error_code ec;
  std::filesystem::create_directories(DataDir(), ec);
  engine.OpenUserStore(userDb);
  engine.StartInBackground(settings);

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
