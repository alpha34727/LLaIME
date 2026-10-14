#pragma once
// Wire protocol between the TIP (in every app) and LLaIME.Engine.exe, over a per-user named
// pipe in message mode. All integers are little-endian; strings are UTF-16 with a u32 length
// in code units. One request message gets exactly one response message.
#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace llaime::protocol {

inline constexpr uint32_t kVersion = 1;

// String SID of the current process's user, used to name per-user objects.
inline std::wstring CurrentUserSid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> buf(size);
  std::wstring sid;
  if (GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
    LPWSTR str = nullptr;
    if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str)) {
      sid = str;
      LocalFree(str);
    }
  }
  CloseHandle(token);
  return sid;
}

// \\.\pipe\LLaIME.Engine.<user SID>
inline std::wstring PipeName(const std::wstring& userSid) {
  return L"\\\\.\\pipe\\LLaIME.Engine." + userSid;
}

enum class MessageType : uint32_t {
  RankRequest = 1,   // context + candidates -> scores
  RankResponse = 2,
  StatusRequest = 3,  // -> model / device information
  StatusResponse = 4,
};

struct RankRequest {
  uint32_t requestId = 0;
  bool privateMode = false;  // the field is a password / private browsing: nothing may be kept
  std::wstring context;      // text before the composition (may be empty)
  std::vector<std::wstring> candidates;
};

struct RankResponse {
  uint32_t requestId = 0;
  bool ok = false;
  std::vector<double> scores;  // natural-log probability per candidate
};

struct StatusResponse {
  bool modelLoaded = false;
  std::wstring model;
  std::wstring device;
};

class Writer {
 public:
  explicit Writer(MessageType type) {
    U32(kVersion);
    U32(static_cast<uint32_t>(type));
  }
  void U32(uint32_t v) { Raw(&v, sizeof(v)); }
  void F64(double v) { Raw(&v, sizeof(v)); }
  void Str(const std::wstring& s) {
    U32(static_cast<uint32_t>(s.size()));
    Raw(s.data(), s.size() * sizeof(wchar_t));
  }
  const std::vector<uint8_t>& bytes() const { return buf_; }

 private:
  void Raw(const void* p, size_t n) {
    auto* b = static_cast<const uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
  }
  std::vector<uint8_t> buf_;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
  // Reads and checks the header; returns the message type.
  std::optional<MessageType> Header() {
    uint32_t version = 0, type = 0;
    if (!U32(&version) || version != kVersion || !U32(&type)) return std::nullopt;
    return static_cast<MessageType>(type);
  }
  bool U32(uint32_t* v) { return Raw(v, sizeof(*v)); }
  bool F64(double* v) { return Raw(v, sizeof(*v)); }
  bool Str(std::wstring* s) {
    uint32_t n = 0;
    if (!U32(&n) || n > (end_ - p_) / sizeof(wchar_t)) return false;
    s->resize(n);
    return Raw(s->data(), n * sizeof(wchar_t));
  }

 private:
  bool Raw(void* out, size_t n) {
    if (static_cast<size_t>(end_ - p_) < n) return false;
    std::memcpy(out, p_, n);
    p_ += n;
    return true;
  }
  const uint8_t* p_;
  const uint8_t* end_;
};

inline std::vector<uint8_t> Encode(const RankRequest& r) {
  Writer w(MessageType::RankRequest);
  w.U32(r.requestId);
  w.U32(r.privateMode ? 1 : 0);
  w.Str(r.context);
  w.U32(static_cast<uint32_t>(r.candidates.size()));
  for (const auto& c : r.candidates) w.Str(c);
  return w.bytes();
}

inline bool Decode(Reader& in, RankRequest* r) {
  uint32_t priv = 0, count = 0;
  if (!in.U32(&r->requestId) || !in.U32(&priv) || !in.Str(&r->context) || !in.U32(&count) ||
      count > 64) {
    return false;
  }
  r->privateMode = priv != 0;
  r->candidates.resize(count);
  for (auto& c : r->candidates) {
    if (!in.Str(&c)) return false;
  }
  return true;
}

inline std::vector<uint8_t> Encode(const RankResponse& r) {
  Writer w(MessageType::RankResponse);
  w.U32(r.requestId);
  w.U32(r.ok ? 1 : 0);
  w.U32(static_cast<uint32_t>(r.scores.size()));
  for (double s : r.scores) w.F64(s);
  return w.bytes();
}

inline bool Decode(Reader& in, RankResponse* r) {
  uint32_t ok = 0, count = 0;
  if (!in.U32(&r->requestId) || !in.U32(&ok) || !in.U32(&count) || count > 64) return false;
  r->ok = ok != 0;
  r->scores.resize(count);
  for (double& s : r->scores) {
    if (!in.F64(&s)) return false;
  }
  return true;
}

inline std::vector<uint8_t> EncodeStatusRequest() { return Writer(MessageType::StatusRequest).bytes(); }

inline std::vector<uint8_t> Encode(const StatusResponse& r) {
  Writer w(MessageType::StatusResponse);
  w.U32(r.modelLoaded ? 1 : 0);
  w.Str(r.model);
  w.Str(r.device);
  return w.bytes();
}

inline bool Decode(Reader& in, StatusResponse* r) {
  uint32_t loaded = 0;
  if (!in.U32(&loaded) || !in.Str(&r->model) || !in.Str(&r->device)) return false;
  r->modelLoaded = loaded != 0;
  return true;
}

}  // namespace llaime::protocol
