#include "EngineClient.h"

#include <filesystem>

#include "Globals.h"

namespace llaime {
namespace {

constexpr wchar_t kClassName[] = L"LLaIME.EngineClient";
constexpr UINT kResponseMessage = WM_APP + 1;
constexpr DWORD kConnectWaitMs = 100;
// Do not try to start the engine more often than this.
constexpr ULONGLONG kLaunchIntervalMs = 30'000;

bool g_classRegistered = false;

// AppContainer processes (Store apps, the Start menu search box) cannot start the engine;
// they only use it when it is already running.
bool IsAppContainer() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD isAppContainer = 0, size = sizeof(isAppContainer);
  GetTokenInformation(token, TokenIsAppContainer, &isAppContainer, size, &size);
  CloseHandle(token);
  return isAppContainer != 0;
}

}  // namespace

void EngineClient::UnregisterClass() {
  if (g_classRegistered && ::UnregisterClassW(kClassName, g_hInst)) g_classRegistered = false;
}

EngineClient::EngineClient(ResponseHandler onResponse)
    : onResponse_(std::move(onResponse)),
      pipeName_(protocol::PipeName(protocol::CurrentUserSid())) {
  if (!g_classRegistered) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &EngineClient::WndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = kClassName;
    g_classRegistered = RegisterClassExW(&wc) != 0;
  }
  window_ = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hInst,
                            this);
  worker_ = std::thread([this] { Run(); });
  DuplicateHandle(GetCurrentProcess(), worker_.native_handle(), GetCurrentProcess(),
                  &workerHandle_, 0, FALSE, DUPLICATE_SAME_ACCESS);
}

EngineClient::~EngineClient() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  // Unblock a pipe read that waits on a busy engine.
  if (workerHandle_) CancelSynchronousIo(workerHandle_);
  if (worker_.joinable()) worker_.join();
  if (workerHandle_) CloseHandle(workerHandle_);
  Disconnect();
  if (window_) DestroyWindow(window_);
}

void EngineClient::Submit(protocol::RankRequest request) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = std::move(request);
  }
  wake_.notify_one();
}

void EngineClient::Run() {
  for (;;) {
    protocol::RankRequest request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
      if (stopping_) return;
      request = std::move(*pending_);
      pending_.reset();
    }
    std::vector<uint8_t> reply;
    protocol::RankResponse response;
    response.requestId = request.requestId;
    if (EnsureConnected() && Transact(protocol::Encode(request), &reply)) {
      protocol::Reader in(reply.data(), reply.size());
      if (in.Header() != protocol::MessageType::RankResponse || !protocol::Decode(in, &response)) {
        response.ok = false;
      }
    }
    if (!response.ok) continue;  // the lexicon conversion already shown stays
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return;
      responses_.push_back(std::move(response));
    }
    PostMessageW(window_, kResponseMessage, 0, 0);
  }
}

bool EngineClient::EnsureConnected() {
  if (pipe_ != INVALID_HANDLE_VALUE) return true;
  for (int attempt = 0; attempt < 2; ++attempt) {
    pipe_ = CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe_ != INVALID_HANDLE_VALUE) {
      DWORD mode = PIPE_READMODE_MESSAGE;
      SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr);
      return true;
    }
    if (GetLastError() == ERROR_PIPE_BUSY) {
      WaitNamedPipeW(pipeName_.c_str(), kConnectWaitMs);
      continue;
    }
    LaunchEngine();
    return false;  // retried on the next keystroke, once the engine is up
  }
  return false;
}

void EngineClient::LaunchEngine() {
  const ULONGLONG now = GetTickCount64();
  if (IsAppContainer() || (lastLaunchTick_ && now - lastLaunchTick_ < kLaunchIntervalMs)) return;
  lastLaunchTick_ = now;
  // <root>\x64\LLaIME.Tip.dll -> <root>\engine\LLaIME.Engine.exe
  wchar_t dllPath[MAX_PATH];
  GetModuleFileNameW(g_hInst, dllPath, MAX_PATH);
  std::filesystem::path exe =
      std::filesystem::path(dllPath).parent_path().parent_path() / L"engine" / L"LLaIME.Engine.exe";
  if (!std::filesystem::exists(exe)) return;
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  // Break away from the host app's job so closing the app does not end the engine.
  const DWORD flags = CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, flags, nullptr,
                      exe.parent_path().c_str(), &si, &pi) &&
      !CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, exe.parent_path().c_str(), &si, &pi)) {
    return;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
}

bool EngineClient::Transact(const std::vector<uint8_t>& request, std::vector<uint8_t>* reply) {
  DWORD written = 0;
  if (!WriteFile(pipe_, request.data(), static_cast<DWORD>(request.size()), &written, nullptr)) {
    Disconnect();
    return false;
  }
  std::vector<uint8_t> buffer(16 * 1024);
  for (;;) {
    DWORD read = 0;
    BOOL ok = ReadFile(pipe_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
    if (!ok && GetLastError() != ERROR_MORE_DATA) {
      Disconnect();
      return false;
    }
    reply->insert(reply->end(), buffer.begin(), buffer.begin() + read);
    if (ok) return true;
  }
}

void EngineClient::Disconnect() {
  if (pipe_ != INVALID_HANDLE_VALUE) {
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
  }
}

LRESULT CALLBACK EngineClient::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  } else if (msg == kResponseMessage) {
    auto* self = reinterpret_cast<EngineClient*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return 0;
    std::deque<protocol::RankResponse> responses;
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      responses.swap(self->responses_);
    }
    for (const auto& r : responses) self->onResponse_(r);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace llaime
