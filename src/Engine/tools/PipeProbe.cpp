// Talks to a running LLaIME.Engine over its named pipe, the way the TIP does: asks for the
// status, then ranks the candidates given on the command line.
//
//   LLaIME.PipeProbe [--context 上文] 候選一 候選二 ...
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "llaime/Protocol.h"

namespace proto = llaime::protocol;

namespace {

std::string Utf8(const std::wstring& s) {
  std::string out(WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr),
                  '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), static_cast<int>(out.size()),
                      nullptr, nullptr);
  out.pop_back();
  return out;
}

bool Transact(HANDLE pipe, const std::vector<uint8_t>& request, std::vector<uint8_t>* reply) {
  reply->assign(64 * 1024, 0);
  DWORD read = 0;
  if (!TransactNamedPipe(pipe, const_cast<uint8_t*>(request.data()),
                         static_cast<DWORD>(request.size()), reply->data(),
                         static_cast<DWORD>(reply->size()), &read, nullptr)) {
    return false;
  }
  reply->resize(read);
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  proto::RankRequest request;
  request.requestId = 1;
  for (int i = 1; i < argc; ++i) {
    std::wstring arg = argv[i];
    if (arg == L"--context" && i + 1 < argc) {
      request.context = argv[++i];
    } else {
      request.candidates.push_back(arg);
    }
  }

  const std::wstring name = proto::PipeName(proto::CurrentUserSid());
  HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "cannot open %ls (error %lu); is LLaIME.Engine running?\n", name.c_str(),
            GetLastError());
    return 1;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

  std::vector<uint8_t> reply;
  if (Transact(pipe, proto::EncodeStatusRequest(), &reply)) {
    proto::Reader in(reply.data(), reply.size());
    proto::StatusResponse status;
    if (in.Header() == proto::MessageType::StatusResponse && proto::Decode(in, &status)) {
      printf("engine: %s on %s (loaded: %s)\n", Utf8(status.model).c_str(),
             Utf8(status.device).c_str(), status.modelLoaded ? "yes" : "no");
    }
  }
  if (request.candidates.size() >= 2) {
    for (int round = 0; round < 2; ++round) {  // the second round is answered from the cache
      const auto start = std::chrono::steady_clock::now();
      if (!Transact(pipe, proto::Encode(request), &reply)) break;
      const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
              .count();
      proto::Reader in(reply.data(), reply.size());
      proto::RankResponse response;
      if (in.Header() != proto::MessageType::RankResponse || !proto::Decode(in, &response)) break;
      printf("round %d: %.1f ms, ok %d\n", round + 1, ms, response.ok);
      for (size_t i = 0; i < response.scores.size(); ++i) {
        printf("  %8.3f  %s\n", response.scores[i], Utf8(request.candidates[i]).c_str());
      }
    }
  }
  CloseHandle(pipe);
  return 0;
}
