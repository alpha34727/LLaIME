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

inline constexpr uint32_t kVersion = 2;

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
  UserDictRequest = 5,  // -> the user's phrases and picks, if changed since `knownVersion`
  UserDictResponse = 6,
  LearnRequest = 7,        // what a commit taught -> Ack
  PhraseRequest = 8,       // add / remove a user phrase -> Ack
  SetLearningRequest = 9,  // pause / resume learning -> Ack
  Ack = 10,
  DevicesRequest = 11,  // -> compute devices the language model can run on
  DevicesResponse = 12,
  ReloadRequest = 13,  // reread engine.ini and load the ranker again -> Ack
};

// Syllables (llaime::Syllable bits) of a reading.
using Reading = std::vector<uint16_t>;

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
  uint32_t userVersion = 0;    // current user dictionary version (see UserDictResponse)
};

struct UserDictRequest {
  uint32_t knownVersion = 0;  // 0: none yet
};

// The whole user dictionary (when `changed`), as built from user.db. `version` changes with
// every edit, by the TIPs or by the settings app.
struct UserDictResponse {
  struct Phrase {
    Reading reading;
    std::wstring text;
    uint32_t kind = 0;  // 0 manual, 1 learned
  };
  struct Selection {
    Reading reading;
    std::wstring text;
    std::wstring prev;
    double weight = 0;  // picks, decayed to now
  };
  uint32_t version = 0;
  bool changed = false;
  bool learningEnabled = true;
  std::vector<Phrase> phrases;
  std::vector<Selection> selections;
};

struct LearnRequest {
  struct Selection {
    Reading reading;
    std::wstring text;
    std::wstring prev;
  };
  struct Phrase {
    Reading reading;
    std::wstring text;
  };
  std::vector<Selection> selections;
  std::vector<Phrase> phrases;
};

struct PhraseRequest {
  enum Op : uint32_t { Add = 1, Remove = 2 };
  uint32_t op = Add;
  Reading reading;
  std::wstring text;
};

struct SetLearningRequest {
  bool enabled = true;
};

struct Ack {
  MessageType request = MessageType::Ack;  // the request this answers
  bool ok = false;
  uint32_t userVersion = 0;
  std::wstring text;  // PhraseRequest: the phrase
};

struct StatusResponse {
  enum State : uint32_t { Loading = 0, Ready = 1, Failed = 2 };
  bool modelLoaded = false;
  std::wstring model;
  std::wstring device;
  // Counts since the engine started (never any text), for tests of privacy mode: rank
  // requests, those flagged private, those that carried context text, and learn requests.
  uint32_t rankRequests = 0;
  uint32_t privateRankRequests = 0;
  uint32_t contextRankRequests = 0;
  uint32_t learnRequests = 0;
  State state = Loading;  // of the ranker
  std::wstring ranker;    // "llm" or "laya" once ready
  std::wstring error;     // why it failed (Failed)
};

struct DevicesResponse {
  struct Device {
    std::wstring name;         // "CPU", "CUDA0", "Vulkan0", ...
    std::wstring description;  // e.g. "NVIDIA GeForce RTX 5060 Ti"
    uint64_t memory = 0;       // bytes
    bool gpu = false;
  };
  std::vector<Device> devices;
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
  void Syllables(const Reading& r) {
    U32(static_cast<uint32_t>(r.size()));
    Raw(r.data(), r.size() * sizeof(uint16_t));
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
  bool Syllables(Reading* r) {
    uint32_t n = 0;
    if (!U32(&n) || n > 64 || n > (end_ - p_) / sizeof(uint16_t)) return false;
    r->resize(n);
    return Raw(r->data(), n * sizeof(uint16_t));
  }
  // A count of items, each at least `minBytes` long, that fits in what is left.
  bool Count(uint32_t* n, size_t minBytes) {
    return U32(n) && *n <= static_cast<size_t>(end_ - p_) / minBytes;
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
  w.U32(r.userVersion);
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
  return in.U32(&r->userVersion);
}

inline std::vector<uint8_t> EncodeStatusRequest() { return Writer(MessageType::StatusRequest).bytes(); }

inline std::vector<uint8_t> Encode(const StatusResponse& r) {
  Writer w(MessageType::StatusResponse);
  w.U32(r.modelLoaded ? 1 : 0);
  w.Str(r.model);
  w.Str(r.device);
  w.U32(r.rankRequests);
  w.U32(r.privateRankRequests);
  w.U32(r.contextRankRequests);
  w.U32(r.learnRequests);
  w.U32(r.state);
  w.Str(r.ranker);
  w.Str(r.error);
  return w.bytes();
}

inline bool Decode(Reader& in, StatusResponse* r) {
  uint32_t loaded = 0;
  if (!in.U32(&loaded) || !in.Str(&r->model) || !in.Str(&r->device) ||
      !in.U32(&r->rankRequests) || !in.U32(&r->privateRankRequests) ||
      !in.U32(&r->contextRankRequests) || !in.U32(&r->learnRequests)) {
    return false;
  }
  r->modelLoaded = loaded != 0;
  uint32_t state = 0;
  if (!in.U32(&state) || state > StatusResponse::Failed || !in.Str(&r->ranker) ||
      !in.Str(&r->error)) {
    return false;
  }
  r->state = static_cast<StatusResponse::State>(state);
  return true;
}

inline std::vector<uint8_t> EncodeDevicesRequest() {
  return Writer(MessageType::DevicesRequest).bytes();
}

inline std::vector<uint8_t> EncodeReloadRequest() {
  return Writer(MessageType::ReloadRequest).bytes();
}

inline std::vector<uint8_t> Encode(const DevicesResponse& r) {
  Writer w(MessageType::DevicesResponse);
  w.U32(static_cast<uint32_t>(r.devices.size()));
  for (const auto& d : r.devices) {
    w.Str(d.name);
    w.Str(d.description);
    w.U32(static_cast<uint32_t>(d.memory & 0xffffffff));
    w.U32(static_cast<uint32_t>(d.memory >> 32));
    w.U32(d.gpu ? 1 : 0);
  }
  return w.bytes();
}

inline bool Decode(Reader& in, DevicesResponse* r) {
  uint32_t count = 0;
  if (!in.Count(&count, 20)) return false;
  r->devices.resize(count);
  for (auto& d : r->devices) {
    uint32_t low = 0, high = 0, gpu = 0;
    if (!in.Str(&d.name) || !in.Str(&d.description) || !in.U32(&low) || !in.U32(&high) ||
        !in.U32(&gpu)) {
      return false;
    }
    d.memory = (static_cast<uint64_t>(high) << 32) | low;
    d.gpu = gpu != 0;
  }
  return true;
}

inline std::vector<uint8_t> Encode(const UserDictRequest& r) {
  Writer w(MessageType::UserDictRequest);
  w.U32(r.knownVersion);
  return w.bytes();
}

inline bool Decode(Reader& in, UserDictRequest* r) { return in.U32(&r->knownVersion); }

inline std::vector<uint8_t> Encode(const UserDictResponse& r) {
  Writer w(MessageType::UserDictResponse);
  w.U32(r.version);
  w.U32(r.changed ? 1 : 0);
  w.U32(r.learningEnabled ? 1 : 0);
  w.U32(static_cast<uint32_t>(r.phrases.size()));
  for (const auto& p : r.phrases) {
    w.Syllables(p.reading);
    w.Str(p.text);
    w.U32(p.kind);
  }
  w.U32(static_cast<uint32_t>(r.selections.size()));
  for (const auto& s : r.selections) {
    w.Syllables(s.reading);
    w.Str(s.text);
    w.Str(s.prev);
    w.F64(s.weight);
  }
  return w.bytes();
}

inline bool Decode(Reader& in, UserDictResponse* r) {
  uint32_t changed = 0, enabled = 0, count = 0;
  if (!in.U32(&r->version) || !in.U32(&changed) || !in.U32(&enabled) || !in.Count(&count, 12)) {
    return false;
  }
  r->changed = changed != 0;
  r->learningEnabled = enabled != 0;
  r->phrases.resize(count);
  for (auto& p : r->phrases) {
    if (!in.Syllables(&p.reading) || !in.Str(&p.text) || !in.U32(&p.kind)) return false;
  }
  if (!in.Count(&count, 20)) return false;
  r->selections.resize(count);
  for (auto& s : r->selections) {
    if (!in.Syllables(&s.reading) || !in.Str(&s.text) || !in.Str(&s.prev) || !in.F64(&s.weight)) {
      return false;
    }
  }
  return true;
}

inline std::vector<uint8_t> Encode(const LearnRequest& r) {
  Writer w(MessageType::LearnRequest);
  w.U32(static_cast<uint32_t>(r.selections.size()));
  for (const auto& s : r.selections) {
    w.Syllables(s.reading);
    w.Str(s.text);
    w.Str(s.prev);
  }
  w.U32(static_cast<uint32_t>(r.phrases.size()));
  for (const auto& p : r.phrases) {
    w.Syllables(p.reading);
    w.Str(p.text);
  }
  return w.bytes();
}

inline bool Decode(Reader& in, LearnRequest* r) {
  uint32_t count = 0;
  if (!in.Count(&count, 12)) return false;
  r->selections.resize(count);
  for (auto& s : r->selections) {
    if (!in.Syllables(&s.reading) || !in.Str(&s.text) || !in.Str(&s.prev)) return false;
  }
  if (!in.Count(&count, 8)) return false;
  r->phrases.resize(count);
  for (auto& p : r->phrases) {
    if (!in.Syllables(&p.reading) || !in.Str(&p.text)) return false;
  }
  return true;
}

inline std::vector<uint8_t> Encode(const PhraseRequest& r) {
  Writer w(MessageType::PhraseRequest);
  w.U32(r.op);
  w.Syllables(r.reading);
  w.Str(r.text);
  return w.bytes();
}

inline bool Decode(Reader& in, PhraseRequest* r) {
  return in.U32(&r->op) && in.Syllables(&r->reading) && in.Str(&r->text);
}

inline std::vector<uint8_t> Encode(const SetLearningRequest& r) {
  Writer w(MessageType::SetLearningRequest);
  w.U32(r.enabled ? 1 : 0);
  return w.bytes();
}

inline bool Decode(Reader& in, SetLearningRequest* r) {
  uint32_t enabled = 0;
  if (!in.U32(&enabled)) return false;
  r->enabled = enabled != 0;
  return true;
}

inline std::vector<uint8_t> Encode(const Ack& r) {
  Writer w(MessageType::Ack);
  w.U32(static_cast<uint32_t>(r.request));
  w.U32(r.ok ? 1 : 0);
  w.U32(r.userVersion);
  w.Str(r.text);
  return w.bytes();
}

inline bool Decode(Reader& in, Ack* r) {
  uint32_t request = 0, ok = 0;
  if (!in.U32(&request) || !in.U32(&ok) || !in.U32(&r->userVersion) || !in.Str(&r->text)) {
    return false;
  }
  r->request = static_cast<MessageType>(request);
  r->ok = ok != 0;
  return true;
}

}  // namespace llaime::protocol

