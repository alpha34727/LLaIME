#pragma once
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>

#include "llaime/Protocol.h"

namespace llaime {

// Talks to LLaIME.Engine.exe from inside an app. All pipe I/O happens on a worker thread so the
// app's UI thread never waits on the model. Rank requests: only the newest is kept (older
// pending ones are superseded by later keystrokes). Other messages (user dictionary, learning,
// phrases) are sent in order, ahead of ranking, and retried for a while if the engine is not up
// yet. Responses are delivered on the thread that created the client, through a message-only
// window.
class EngineClient {
 public:
  struct Handlers {
    std::function<void(const protocol::RankResponse&)> rank;
    std::function<void(const protocol::UserDictResponse&)> userDict;
    std::function<void(const protocol::Ack&)> ack;
  };

  explicit EngineClient(Handlers handlers);
  ~EngineClient();
  EngineClient(const EngineClient&) = delete;
  EngineClient& operator=(const EngineClient&) = delete;

  void Submit(protocol::RankRequest request);
  // An encoded request other than ranking.
  void Send(std::vector<uint8_t> message);

  static void UnregisterClass();

 private:
  using Response =
      std::variant<protocol::RankResponse, protocol::UserDictResponse, protocol::Ack>;
  struct Queued {
    std::vector<uint8_t> message;
    int attempts = 0;
  };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void Run();
  bool EnsureConnected();
  void LaunchEngine();
  bool Transact(const std::vector<uint8_t>& request, std::vector<uint8_t>* reply);
  void Disconnect();
  void Deliver(Response response);

  Handlers handlers_;
  HWND window_ = nullptr;
  std::thread worker_;
  HANDLE workerHandle_ = nullptr;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<protocol::RankRequest> pending_;
  std::deque<Queued> queue_;
  std::deque<Response> responses_;
  bool stopping_ = false;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::wstring pipeName_;
  ULONGLONG lastLaunchTick_ = 0;
};

}  // namespace llaime
