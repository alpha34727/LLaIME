#include <gtest/gtest.h>

#include "llaime/Syllable.h"

using llaime::Syllable;

TEST(Syllable, RoundTrip) {
  for (const wchar_t* text : {L"ㄧˋ", L"ㄓ", L"ㄓㄨㄤˋ", L"ㄉㄜ˙", L"ㄩㄝˋ", L"ㄦˊ", L"ㄅㄚ"}) {
    auto s = Syllable::Parse(text);
    ASSERT_TRUE(s.has_value()) << text;
    EXPECT_EQ(s->ToString(), text);
  }
}

TEST(Syllable, UnmarkedIsToneOne) {
  auto s = Syllable::Parse(L"ㄇㄚ");
  ASSERT_TRUE(s);
  EXPECT_EQ(s->tone(), 1);
  EXPECT_FALSE(s->withoutTone().hasTone());
}

TEST(Syllable, RejectsInvalid) {
  EXPECT_FALSE(Syllable::Parse(L""));
  EXPECT_FALSE(Syllable::Parse(L"ˋ"));
  EXPECT_FALSE(Syllable::Parse(L"a"));
}
