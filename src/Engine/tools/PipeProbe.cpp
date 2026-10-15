// Talks to a running LLaIME.Engine over its named pipe, the way the TIP does: asks for the
// status, then ranks the candidates given on the command line.
//
//   LLaIME.PipeProbe [--context 上文] 候選一 候選二 ...
//   LLaIME.PipeProbe --add ㄅㄛˊ-ㄩㄣˊ 柏昀       add a user phrase
//   LLaIME.PipeProbe --pick ㄧˋ-ㄧˋ 異議 [前詞]   learn a pick, as a commit would
//   LLaIME.PipeProbe --dict                       print the user dictionary
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "llaime/Protocol.h"
#include "llaime/UserDictionary.h"

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
  reply->assign(4 * 1024 * 1024, 0);
  DWORD read = 0;
  if (!TransactNamedPipe(pipe, const_cast<uint8_t*>(request.data()),
                         static_cast<DWORD>(request.size()), reply->data(),
                         static_cast<DWORD>(reply->size()), &read, nullptr)) {
    return false;
  }
  reply->resize(read);
  return true;
}

proto::Reading WireReading(const std::wstring& text) {
  proto::Reading out;
  if (auto syllables = llaime::ParseReading(text)) {
    for (auto s : *syllables) out.push_back(s.bits());
  }
  return out;
}

std::wstring ReadingText(const proto::Reading& reading) {
  std::vector<llaime::Syllable> syllables;
  for (uint16_t b : reading) syllables.push_back(llaime::Syllable(b));
  return llaime::FormatReading(syllables);
}

void PrintAck(const std::vector<uint8_t>& reply) {
  proto::Reader in(reply.data(), reply.size());
  proto::Ack ack;
  if (in.Header() == proto::MessageType::Ack && proto::Decode(in, &ack)) {
    printf("ack: ok %d, user dictionary version %u\n", ack.ok, ack.userVersion);
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  proto::RankRequest request;
  request.requestId = 1;
  std::vector<std::vector<uint8_t>> commands;
  bool dict = false;
  for (int i = 1; i < argc; ++i) {
    std::wstring arg = argv[i];
    if (arg == L"--context" && i + 1 < argc) {
      request.context = argv[++i];
    } else if (arg == L"--add" && i + 2 < argc) {
      proto::PhraseRequest add;
      add.reading = WireReading(argv[i + 1]);
      add.text = argv[i + 2];
      commands.push_back(proto::Encode(add));
      i += 2;
    } else if (arg == L"--pick" && i + 2 < argc) {
      proto::LearnRequest learn;
      std::wstring prev = i + 3 < argc && argv[i + 3][0] != L'-' ? argv[i + 3] : L"";
      learn.selections.push_back({WireReading(argv[i + 1]), argv[i + 2], prev});
      commands.push_back(proto::Encode(learn));
      i += prev.empty() ? 2 : 3;
    } else if (arg == L"--dict") {
      dict = true;
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
  for (const auto& command : commands) {
    if (Transact(pipe, command, &reply)) PrintAck(reply);
  }
  if (dict && Transact(pipe, proto::Encode(proto::UserDictRequest{}), &reply)) {
    proto::Reader in(reply.data(), reply.size());
    proto::UserDictResponse r;
    if (in.Header() == proto::MessageType::UserDictResponse && proto::Decode(in, &r)) {
      printf("user dictionary version %u, learning %s\n", r.version,
             r.learningEnabled ? "on" : "paused");
      for (const auto& p : r.phrases) {
        printf("  phrase  %s %s (%s)\n", Utf8(ReadingText(p.reading)).c_str(),
               Utf8(p.text).c_str(), p.kind ? "learned" : "manual");
      }
      for (const auto& s : r.selections) {
        printf("  pick    %s %s after '%s' weight %.2f\n", Utf8(ReadingText(s.reading)).c_str(),
               Utf8(s.text).c_str(), Utf8(s.prev).c_str(), s.weight);
      }
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
