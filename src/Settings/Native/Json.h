#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace llaime {

// Just enough JSON output for the settings bridge: objects and arrays written in order.
class JsonWriter {
 public:
  JsonWriter& BeginObject() { return Open(L'{'); }
  JsonWriter& EndObject() { return Close(L'}'); }
  JsonWriter& BeginArray() { return Open(L'['); }
  JsonWriter& EndArray() { return Close(L']'); }

  JsonWriter& Key(std::wstring_view key) {
    Separate();
    String(key);
    out_ += L':';
    afterKey_ = true;
    return *this;
  }
  JsonWriter& Value(std::wstring_view s) {
    Separate();
    String(s);
    return *this;
  }
  JsonWriter& Value(const wchar_t* s) { return Value(std::wstring_view(s)); }
  JsonWriter& Value(bool b) {
    Separate();
    out_ += b ? L"true" : L"false";
    return *this;
  }
  JsonWriter& Value(int64_t n) {
    Separate();
    out_ += std::to_wstring(n);
    return *this;
  }
  JsonWriter& Value(uint64_t n) {
    Separate();
    out_ += std::to_wstring(n);
    return *this;
  }
  JsonWriter& Value(int n) { return Value(static_cast<int64_t>(n)); }
  JsonWriter& Value(uint32_t n) { return Value(static_cast<uint64_t>(n)); }
  JsonWriter& Value(double d) {
    Separate();
    wchar_t buf[32];
    swprintf_s(buf, L"%.6g", d);
    out_ += buf;
    return *this;
  }
  template <typename T>
  JsonWriter& Field(std::wstring_view key, T value) {
    Key(key);
    return Value(value);
  }

  const std::wstring& str() const { return out_; }

 private:
  JsonWriter& Open(wchar_t c) {
    Separate();
    out_ += c;
    first_ = true;
    return *this;
  }
  JsonWriter& Close(wchar_t c) {
    out_ += c;
    first_ = false;
    return *this;
  }
  void Separate() {
    if (afterKey_) {
      afterKey_ = false;
      return;
    }
    if (!first_ && !out_.empty()) out_ += L',';
    first_ = false;
  }
  void String(std::wstring_view s) {
    out_ += L'"';
    for (wchar_t c : s) {
      switch (c) {
        case L'"': out_ += L"\\\""; break;
        case L'\\': out_ += L"\\\\"; break;
        case L'\n': out_ += L"\\n"; break;
        case L'\r': out_ += L"\\r"; break;
        case L'\t': out_ += L"\\t"; break;
        default:
          if (c < 0x20) {
            wchar_t buf[8];
            swprintf_s(buf, L"\\u%04x", c);
            out_ += buf;
          } else {
            out_ += c;
          }
      }
    }
    out_ += L'"';
  }

  std::wstring out_;
  bool first_ = true;
  bool afterKey_ = false;
};

}  // namespace llaime
