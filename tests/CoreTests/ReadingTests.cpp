#include <gtest/gtest.h>

#include <string>

#include "llaime/BopomofoReading.h"

using namespace llaime;
using Result = BopomofoReading::Result;

namespace {

// Types `keys` on the standard layout and returns the completed syllables, space-separated.
std::wstring TypeStandard(const std::wstring& keys) {
  BopomofoReading reading(CreateStandardLayout());
  std::wstring out;
  for (wchar_t k : keys) {
    if (reading.Feed(k) == Result::Completed) {
      if (!out.empty()) out += L' ';
      out += reading.TakeSyllable().ToString();
    }
  }
  return out;
}

}  // namespace

TEST(StandardLayout, Words) {
  EXPECT_EQ(TypeStandard(L"u4u4"), L"ㄧˋ ㄧˋ");          // 意義
  EXPECT_EQ(TypeStandard(L"5j/ 5ji4"), L"ㄓㄨㄥ ㄓㄨㄛˋ");  // ㄓㄨㄥ ㄓㄨㄛˋ
  EXPECT_EQ(TypeStandard(L"m,4"), L"ㄩㄝˋ");             // 月
  EXPECT_EQ(TypeStandard(L"2k7"), L"ㄉㄜ˙");             // 的
  EXPECT_EQ(TypeStandard(L"5 "), L"ㄓ");                 // 之
  EXPECT_EQ(TypeStandard(L"-6"), L"ㄦˊ");                // 兒
  EXPECT_EQ(TypeStandard(L"ji3"), L"ㄨㄛˇ");             // 我
  EXPECT_EQ(TypeStandard(L"z8 "), L"ㄈㄚ");              // 發
}

TEST(BopomofoReading, ReplacesSameComponent) {
  BopomofoReading reading(CreateStandardLayout());
  reading.Feed(L'1');  // ㄅ
  reading.Feed(L'q');  // ㄆ replaces ㄅ
  reading.Feed(L'8');  // ㄚ
  EXPECT_EQ(reading.Text(), L"ㄆㄚ");
}

TEST(BopomofoReading, ToneOnEmptyIsIgnored) {
  BopomofoReading reading(CreateStandardLayout());
  EXPECT_EQ(reading.Feed(L'4'), Result::Ignored);
  EXPECT_EQ(reading.Feed(L'!'), Result::NotHandled);
}

TEST(BopomofoReading, Backspace) {
  BopomofoReading reading(CreateStandardLayout());
  reading.Feed(L'5');
  reading.Feed(L'j');
  reading.Feed(L'/');
  EXPECT_TRUE(reading.Backspace());
  EXPECT_EQ(reading.Text(), L"ㄓㄨ");
  EXPECT_TRUE(reading.Backspace());
  EXPECT_TRUE(reading.Backspace());
  EXPECT_TRUE(reading.Empty());
  EXPECT_FALSE(reading.Backspace());
}
