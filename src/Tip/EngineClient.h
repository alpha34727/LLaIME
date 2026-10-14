#pragma once
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "llaime/Protocol.h"

namespace llaime {

// Talks to LLaIME.Engine.exe from inside an app. All pipe I/O happens on a worker thread so the
// app's UI thread never waits on the model; only the newest request is kept (older pending ones
// are superseded by later keystrokes). Responses are delivered on the thread that created the
// client, through a message-only window.
class EngineClient {
 public:
  using ResponseHandler = std::function<void(const protocol::RankResponse&)>;

  explicit EngineClient(ResponseHandler onResponse);
  ~EngineClient();
  EngineClient(const EngineClient&) = delete;
  EngineClient& operator=(const EngineClient&) = delete;

  void Submit(protocol::RankRequest request);

  static void UnregisterClass();

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void Run();
  bool EnsureConnected();
  void LaunchEngine();
  bool Transact(const std::vector<uint8_t>& request, std::vector<uint8_t>* reply);
  void Disconnect();

  ResponseHandler onResponse_;
  HWND window_ = nullptr;
  std::thread worker_;
  HANDLE workerHandle_ = nullptr;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<protocol::RankRequest> pending_;
  std::deque<protocol::RankResponse> responses_;
  bool stopping_ = false;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::wstring pipeName_;
  ULONGLONG lastLaunchTick_ = 0;
};

}  // namespace llaime
